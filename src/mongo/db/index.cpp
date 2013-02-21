/** @file index.cpp */

/**
*    Copyright (C) 2008 10gen Inc.
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

#include <boost/checked_delete.hpp>
#include <db.h>

#include "mongo/db/namespace.h"
#include "mongo/db/index.h"
#include "mongo/db/cursor.h"
#include "mongo/db/background.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/ops/delete.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    IndexDetails::IndexDetails(const BSONObj &info, bool may_create) : _info(info.getOwned()) {
        string dbname = indexNamespace();
        tokulog() << "Opening IndexDetails " << dbname << endl;
        // Open the dictionary. Creates it if necessary.
        int r = storage::db_open(&_db, dbname, keyPattern(), may_create);
        verify(r == 0);
        if (may_create) {
            addNewNamespaceToCatalog(dbname);
        }
    }

    IndexDetails::~IndexDetails() {
        tokulog() << "Closing IndexDetails " << indexNamespace() << endl;
        storage::db_close(_db);
    }

    int IndexDetails::keyPatternOffset( const string& key ) const {
        BSONObjIterator i( keyPattern() );
        int n = 0;
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( key == e.fieldName() )
                return n;
            n++;
        }
        return -1;
    }

    /* delete this index.  does NOT clean up the system catalog
       (system.indexes or system.namespaces) -- only NamespaceIndex.
    */
    void IndexDetails::kill_idx() {
        string ns = indexNamespace(); // e.g. foo.coll.$ts_1
        try {

            string pns = parentNS(); // note we need a copy, as parentNS() won't work after the drop() below

            // clean up parent namespace index cache
            NamespaceDetailsTransient::get( pns.c_str() ).deletedIndex();

            string name = indexName();
            
            // tokudb: ensure the db is dropped in the environment using dropIndex
            //idxInterface().dropIndex(*this);

            /* important to catch exception here so we can finish cleanup below. */
            try {
                ::abort();
                //dropNS(ns.c_str());
            }
            catch(DBException& ) {
                log(2) << "IndexDetails::kill(): couldn't drop ns " << ns << endl;
            }
            ::abort();
#if 0
            head.setInvalid();
            info.setInvalid();

            // clean up in system.indexes.  we do this last on purpose.
            int n = removeFromSysIndexes(pns.c_str(), name.c_str());
            wassert( n == 1 );
#endif

        }
        catch ( DBException &e ) {
            log() << "exception in kill_idx: " << e << ", ns: " << ns << endl;
        }
    }

    void IndexDetails::getKeysFromObject( const BSONObj& obj, BSONObjSet& keys) const {
        getSpec().getKeys( obj, keys );
    }

    const IndexSpec& IndexDetails::getSpec() const {
        SimpleMutex::scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
        return NamespaceDetailsTransient::get_inlock( info()["ns"].valuestr() ).getIndexSpec( this );
    }

    void IndexDetails::insert(const BSONObj &obj, const BSONObj &primary_key, bool overwrite) {
        BSONObjSet keys;
        getKeysFromObject(obj, keys);
        if (keys.size() > 1) {
            const char *ns = parentNS().c_str();
            NamespaceDetails *d = nsdetails(ns);
            const int idxNo = d->idxNo(*this);
            dassert(idxNo >= 0);
            d->setIndexIsMultikey(ns, idxNo);
        }

        for (BSONObjSet::const_iterator ki = keys.begin(); ki != keys.end(); ++ki) {
            if (isIdIndex()) {
                insertPair(*ki, NULL, obj, overwrite);
            } else if (clustering()) {
                insertPair(*ki, &primary_key, obj, overwrite);
            } else {
                insertPair(*ki, &primary_key, BSONObj(), overwrite);
            }
        }
    }

    void IndexDetails::insertPair(const BSONObj &key, const BSONObj *pk, const BSONObj &val, bool overwrite) {
        const int buflen = key.objsize() + (pk != NULL ? pk->objsize() : 0);
        char buf[buflen];
        memcpy(buf, key.objdata(), key.objsize());
        if (pk != NULL) {
            memcpy(buf + key.objsize(), pk->objdata(), pk->objsize());
        }
        DBT kdbt, vdbt;
        kdbt.data = const_cast<void *>(static_cast<const void *>(buf));
        kdbt.size = buflen;
        vdbt.data = const_cast<void *>(static_cast<const void *>(val.objdata()));
        vdbt.size = val.objsize();
        const int flags = (unique() && !overwrite) ? DB_NOOVERWRITE : 0;
        int r = _db->put(_db, cc().transaction().txn(), &kdbt, &vdbt, flags);
        uassert(16433, "key already exists in unique index", r != DB_KEYEXIST);
        if (r != 0) {
            tokulog() << "error inserting " << key << ", " << val << endl;
        } else {
            tokulog(1) << "index " << info()["key"].Obj() << ": inserted " << key << ", pk " << (pk ? *pk : BSONObj()) << ", val " << val << endl;
        }
        verify(r == 0);
    }

    void IndexDetails::deleteObject(const BSONObj &pk, const BSONObj &obj) {
        BSONObjSet keys;
        getKeysFromObject(obj, keys);
        for (BSONObjSet::const_iterator ki = keys.begin(); ki != keys.end(); ++ki) {
            const BSONObj &key = *ki;
            const int buflen = key.objsize() + (isIdIndex() ? 0 : pk.objsize());
            char buf[buflen];
            memcpy(buf, key.objdata(), key.objsize());
            if (!isIdIndex()) {
                memcpy(buf + key.objsize(), pk.objdata(), pk.objsize());
            }
            DBT kdbt;
            kdbt.data = const_cast<void *>(static_cast<const void *>(buf));
            kdbt.size = buflen;
            const int flags = DB_DELETE_ANY;
            int r = _db->del(_db, cc().transaction().txn(), &kdbt, flags);
            verify(r == 0);
        }
    }

    // Get a DBC over an index. Must already be in the context of a transction.
    DBC *IndexDetails::cursor() const {
        DBC *cursor;
        const Client::Transaction &txn = cc().transaction();
        int r = _db->cursor(_db, txn.txn(), &cursor, 0);
        verify(r == 0);
        return cursor;
    }

    void IndexSpec::reset( const IndexDetails * details ) {
        _details = details;
        reset( details->info() );
    }

    void IndexSpec::reset( const BSONObj& _info ) {
        info = _info;
        keyPattern = info["key"].Obj();
        if ( keyPattern.objsize() == 0 ) {
            out() << info.toString() << endl;
            verify(false);
        }
        _init();
    }
}
