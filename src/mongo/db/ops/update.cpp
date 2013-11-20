//@file update.cpp

/**
 *    Copyright (C) 2008 10gen Inc.
 *    Copyright (C) 2013 Tokutek Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"

#include "mongo/db/oplog.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/query_optimizer.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/query.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_internal.h"
#include "mongo/db/oplog_helpers.h"

namespace mongo {

    void updateOneObject(NamespaceDetails *d, const BSONObj &pk, 
                         const BSONObj &updateobj,
                         const BSONObj &oldObj, const BSONObj &newObj, 
                         const bool logop, const bool fromMigrate,
                         uint64_t flags) {
        if (!updateobj.isEmpty()) {
            cc().curop()->debug().fastmod = true;
            d->updateObjectMods(pk, updateobj, logop, fromMigrate, flags);
        } else {
            d->updateObject(pk, oldObj, newObj, logop, fromMigrate, flags);
        }
        d->notifyOfWriteOp();
    }

    static void checkNoMods(const BSONObj &obj) {
        for (BSONObjIterator i(obj); i.more(); ) {
            const BSONElement &e = i.next();
            uassert(10154, "Modifiers and non-modifiers cannot be mixed", e.fieldName()[0] != '$');
        }
    }

    static void checkTooLarge(const BSONObj &obj) {
        uassert(12522, "$ operator made object too large", obj.objsize() <= BSONObjMaxUserSize);
    }

    // Apply an update message supplied by a NamespaceDetails to
    // some row in an in IndexDetails (for fast ydb updates).
    class ApplyUpdateMessage : public storage::UpdateCallback {
        BSONObj apply(const BSONObj &oldObj, const BSONObj &msg) {
            try {
                // The update message is simply an update object, supplied by the user.
                const BSONObj &updateObj = msg;
                ModSet mods( updateObj );
                auto_ptr<ModSetState> mss = mods.prepare( oldObj );
                BSONObj newObj = mss->createNewFromMods();
                checkTooLarge( newObj );
                return newObj;
            } catch (...) {
                // Applying an update message in this fashion _always_ ignores errors.
                // That is the risk you take when using --fastupdates.
                return oldObj;
            }
        }
    } _storageUpdateCallback; // installed as the ydb update callback in db.cpp via set_update_callback

    static void updateUsingMods(NamespaceDetails *d, const BSONObj &pk, const BSONObj &obj,
                                ModSetState &mss, const bool modsAreIndexed,
                                const bool logop, const bool fromMigrate) {
        const BSONObj newObj = mss.createNewFromMods();
        checkTooLarge(newObj);
        updateOneObject(d, pk, BSONObj(), obj, newObj, logop, fromMigrate,
                        modsAreIndexed ? 0 : NamespaceDetails::KEYS_UNAFFECTED_HINT);
    }

    static void updateNoMods(NamespaceDetails *d, const BSONObj &pk, const BSONObj &obj, const BSONObj &updateobj,
                             const bool logop, const bool fromMigrate) {
        // This is incredibly un-intiutive, but it takes a const BSONObj
        // and modifies it in-place if a timestamp needs to be set.
        BSONElementManipulator::lookForTimestamps(updateobj);
        checkNoMods(updateobj);
        updateOneObject(d, pk, BSONObj(), obj, updateobj, logop, fromMigrate);
    }

    static UpdateResult upsertAndLog(NamespaceDetails *d, const BSONObj &patternOrig,
                                     const BSONObj &updateobj, const bool isOperatorUpdate,
                                     ModSet *mods, const bool logop) {
        const string &ns = d->ns();
        uassert(16893, str::stream() << "Cannot update a collection under-going bulk load: " << ns,
                       ns != cc().bulkLoadNS());

        BSONObj newObj = updateobj;
        if (isOperatorUpdate) {
            newObj = mods->createNewFromQuery(patternOrig);
            cc().curop()->debug().fastmodinsert = true;
        } else {
            cc().curop()->debug().upsert = true;
        }

        checkNoMods(newObj);
        insertOneObject(d, newObj);
        if (logop) {
            OpLogHelpers::logInsert(ns.c_str(), newObj);
        }
        return UpdateResult(0, isOperatorUpdate, 1, newObj);
    }

    static bool hasClusteringSecondaryKey(NamespaceDetails *d) {
        for (int i = 0; i < d->nIndexesBeingBuilt(); i++) {
            IndexDetails &idx = d->idx(i);
            if (!d->isPKIndex(idx) && idx.clustering()) {
                // has a clustering secondary key
                return true;
            }
        }
        // no clustering secondary keys
        return false;
    }

    static UpdateResult _updateById(NamespaceDetails *d,
                                    const BSONObj &patternOrig, const BSONObj &idQuery,
                                    const BSONObj &updateobj, const bool isOperatorUpdate,
                                    ModSet *mods, const bool logop, const bool fromMigrate) {
        if (cmdLine.fastupdates && !mods->isIndexed() &&
            !hasClusteringSecondaryKey(d)) {
            // Fast update path that skips the _id query.
            // We know no indexes need to be updated so we don't read the full object.
            //
            // Further, we specifically do _not_ check if upsert is true because it's
            // implied when using fastupdates.
            const BSONObj &pk = idQuery.firstElement().wrap("");
            updateOneObject(d, pk, updateobj, BSONObj(), BSONObj(), logop, fromMigrate);
            return UpdateResult(0, 0, 1, BSONObj());
        }

        BSONObj obj;
        ResultDetails queryResult;
        if (mods && mods->hasDynamicArray()) {
            queryResult.matchDetails.requestElemMatchKey();
        }

        const bool found = queryByIdHack(d, idQuery, patternOrig, obj, &queryResult);
        if (!found) {
            // no upsert support in _updateById yet, so we are done.
            return UpdateResult(0, 0, 0, BSONObj());
        }

        const BSONObj &pk = idQuery.firstElement().wrap("");
        if (isOperatorUpdate) {
            // operator-style update
            ModSet *useMods = mods;
            auto_ptr<ModSet> mymodset;
            if (queryResult.matchDetails.hasElemMatchKey() && mods->hasDynamicArray()) {
                useMods = mods->fixDynamicArray( queryResult.matchDetails.elemMatchKey());
                mymodset.reset(useMods);
            }
            auto_ptr<ModSetState> mss = useMods->prepare(obj, false /* not an insertion */);
            updateUsingMods(d, pk, obj, *mss, useMods->isIndexed() > 0, logop, fromMigrate);
        } else {
            // replace-style update
            updateNoMods(d, pk, obj, updateobj, logop, fromMigrate);
        }
        return UpdateResult(1, isOperatorUpdate, 1, BSONObj());
    }

    static UpdateResult _updateObjects(const char *ns,
                                       const BSONObj &updateobj,
                                       const BSONObj &patternOrig,
                                       const bool upsert, const bool multi,
                                       const bool logop, const bool fromMigrate) {
        TOKULOG(2) << "update: " << ns
                   << " update: " << updateobj
                   << " query: " << patternOrig
                   << " upsert: " << upsert << " multi: " << multi << endl;

        NamespaceDetails *d = getAndMaybeCreateNS(ns, logop);

        auto_ptr<ModSet> mods;
        const bool isOperatorUpdate = updateobj.firstElementFieldName()[0] == '$';
        if (isOperatorUpdate) {
            mods.reset(new ModSet(updateobj, d->indexKeys()));
        }

        // Attempt to take the fast path for updates by _id
        if (d->mayFindById()) {
            const BSONObj idQuery = getSimpleIdQuery(patternOrig);
            if (!idQuery.isEmpty()) {
                UpdateResult result = _updateById(d, patternOrig, idQuery,
                                                  updateobj, isOperatorUpdate, mods.get(),
                                                  logop, fromMigrate);
                if (result.existing || !upsert) {
                    return result;
                }
            }
        }

        // Run a regular update using the query optimizer.

        int numModded = 0;
        cc().curop()->debug().nscanned = 0;
        set<BSONObj> seenObjects;
        MatchDetails details;
        if (mods.get() && mods->hasDynamicArray()) {
            details.requestElemMatchKey();
        }
        for (shared_ptr<Cursor> c = getOptimizedCursor(ns, patternOrig); c->ok(); ) {
            cc().curop()->debug().nscanned++;
            BSONObj currPK = c->currPK();
            if (c->getsetdup(currPK)) {
                c->advance();
                continue;
            }
            if (!c->currentMatches(&details)) {
                c->advance();
                continue;
            }

            BSONObj currentObj = c->current();
            if (!isOperatorUpdate) {
                // replace-style update only affects a single matching document
                uassert(10158, "multi update only works with $ operators", !multi);
                updateNoMods(d, currPK, currentObj, updateobj, logop, fromMigrate);
                return UpdateResult(1, 0, 1, BSONObj());
            }

            // operator-style updates may affect many documents
            if (multi) {
                // Advance past the document to be modified - SERVER-5198,
                // First, get owned copies of currPK/currObj, which live in the cursor.
                currPK = currPK.getOwned();
                currentObj = currentObj.getOwned();
                while (c->ok() && currPK == c->currPK()) {
                    c->advance();
                }

                // Multi updates need to do their own deduplication because updates may modify the
                // keys the cursor is in the process of scanning over.
                if ( seenObjects.count(currPK) ) {
                    continue;
                } else {
                    seenObjects.insert(currPK);
                }
            }

            ModSet *useMods = mods.get();
            auto_ptr<ModSet> mymodset;
            if (details.hasElemMatchKey() && mods->hasDynamicArray()) {
                useMods = mods->fixDynamicArray(details.elemMatchKey());
                mymodset.reset(useMods);
            }
            auto_ptr<ModSetState> mss = useMods->prepare(currentObj, false /* not an insertion */);
            updateUsingMods(d, currPK, currentObj, *mss, useMods->isIndexed() > 0, logop, fromMigrate);
            numModded++;

            if (!multi) {
                break;
            }
        }

        if (numModded) {
            // We've modified something, so we're done.
            return UpdateResult(1, 1, numModded, BSONObj());
        }
        if (!upsert) {
            // We haven't modified anything, but we're not trying to upsert, so we're done.
            return UpdateResult(0, isOperatorUpdate, numModded, BSONObj());
        }

        if (!isOperatorUpdate) {
            uassert(10159, "multi update only works with $ operators", !multi);
        }
        // Upsert a new object
        return upsertAndLog(d, patternOrig, updateobj, isOperatorUpdate, mods.get(), logop);
    }

    UpdateResult updateObjects(const char *ns,
                               const BSONObj &updateobj, const BSONObj &patternOrig,
                               const bool upsert, const bool multi,
                               const bool logop, const bool fromMigrate) {
        uassert(10155, "cannot update reserved $ collection", NamespaceString::normal(ns));
        if (NamespaceString::isSystem(ns)) {
            uassert(10156, str::stream() << "cannot update system collection: " << ns <<
                                            " q: " << patternOrig << " u: " << updateobj,
                           legalClientSystemNS(ns , true));
        }

        cc().curop()->debug().updateobj = updateobj;

        UpdateResult ur = _updateObjects(ns, updateobj, patternOrig,
                                         upsert, multi, logop, fromMigrate);

        cc().curop()->debug().nupdated = ur.num;
        return ur;
    }

}  // namespace mongo
