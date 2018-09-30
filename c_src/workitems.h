// -------------------------------------------------------------------
//
// eleveldb: Erlang Wrapper for LevelDB (http://code.google.com/p/leveldb/)
//
// Copyright (c) 2011-2015 Basho Technologies, Inc. All Rights Reserved.
//
// This file is provided to you under the Apache License,
// Version 2.0 (the "License"); you may not use this file
// except in compliance with the License.  You may obtain
// a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// -------------------------------------------------------------------

#ifndef INCL_WORKITEMS_H
#define INCL_WORKITEMS_H

#include <stdint.h>

#include "leveldb/db.h"
#include "leveldb/write_batch.h"
#define LEVELDB_PLATFORM_POSIX
#include "port/port.h"
#include "util/mutexlock.h"
#include "util/thread_tasks.h"

#ifndef __WORK_RESULT_HPP
    #include "work_result.hpp"
#endif

#ifndef ATOMS_H
    #include "atoms.h"
#endif

#ifndef INCL_REFOBJECTS_H
    #include "refobjects.h"
#endif

namespace eleveldb {

class Signal {
    leveldb::port::Mutex lock;
    leveldb::port::CondVar cv;
    bool signal;

  public:

    Signal();
    ~Signal();

    void Set();
    void Wait(bool clear = true);
};

/* Type returned from a work task: */
typedef basho::async_nif::work_result   work_result;



/**
 * Virtual base class for async NIF work items:
 */
class WorkTask : public leveldb::ThreadTask
{
 protected:
    ReferencePtr<DbObject> m_DbPtr;             //!< access to database, and holds reference

    ErlNifEnv      *local_env_;
    ERL_NIF_TERM   caller_ref_term;
    ERL_NIF_TERM   caller_pid_term;
    bool           terms_set;

    ErlNifPid local_pid;   // maintain for task lifetime (JFW)

    Signal * to_notify;

 public:
    WorkTask(ErlNifEnv *caller_env, ERL_NIF_TERM& caller_ref);

    WorkTask(ErlNifEnv *caller_env, ERL_NIF_TERM& caller_ref, DbObjectPtr_t & DbPtr);

    void SetToNotify(Signal * signal);

    virtual ~WorkTask();

    // this is the method called from the thread pool's worker thread; it
    // calls DoWork(), implemented in the subclass, and returns the result
    // of the work to the caller
    virtual void operator()();

    virtual ErlNifEnv *local_env()         { return local_env_; }

    // call local_env() since the virtual creates the data in MoveTask
    const ERL_NIF_TERM& caller_ref()       { local_env(); return caller_ref_term; }
    const ERL_NIF_TERM& pid()              { local_env(); return caller_pid_term; }

 protected:

    // this is the method that does the real work for this task
    virtual work_result DoWork() = 0;

 private:
    WorkTask();
    WorkTask(const WorkTask &);
    WorkTask & operator=(const WorkTask &);

};  // class WorkTask


/**
 * Background object for async open of a leveldb instance
 */

class OpenTask : public WorkTask
{
protected:
    std::string         db_name;
    leveldb::Options   *open_options;  // associated with db handle, we don't free it

public:
    OpenTask(ErlNifEnv* caller_env, ERL_NIF_TERM& _caller_ref,
             const std::string& db_name_, leveldb::Options *open_options_);

    virtual ~OpenTask() {};

protected:
    virtual work_result DoWork();

private:
    OpenTask();
    OpenTask(const OpenTask &);
    OpenTask & operator=(const OpenTask &);

};  // class OpenTask



/**
 * Background object for async write
 */

class WriteTask : public WorkTask
{
protected:
    leveldb::WriteBatch*    batch;
    leveldb::WriteOptions*          options;

public:

    WriteTask(ErlNifEnv* _owner_env, ERL_NIF_TERM _caller_ref,
                DbObjectPtr_t & _db_handle,
                leveldb::WriteBatch* _batch,
                leveldb::WriteOptions* _options)
        : WorkTask(_owner_env, _caller_ref, _db_handle),
       batch(_batch),
       options(_options)
    {}

    virtual ~WriteTask()
    {
        delete batch;
        delete options;
    }

protected:
    virtual work_result DoWork()
    {
        leveldb::Status status = m_DbPtr->m_Db->Write(*options, batch);

        return (status.ok() ? work_result(ATOM_OK) : work_result(local_env(), ATOM_ERROR_DB_WRITE, status));
    }

};  // class WriteTask


/**
 * Alternate object for retrieving data out of leveldb.
 *  Reduces one memcpy operation.
 */
class BinaryValue : public leveldb::Value
{
private:
    ErlNifEnv* m_env;
    ERL_NIF_TERM& m_value_bin;

    BinaryValue(const BinaryValue&);
    void operator=(const BinaryValue&);

public:

    BinaryValue(ErlNifEnv* env, ERL_NIF_TERM& value_bin)
    : m_env(env), m_value_bin(value_bin)
    {};

    virtual ~BinaryValue() {};

    BinaryValue & assign(const char* data, size_t size)
    {
        unsigned char* v = enif_make_new_binary(m_env, size, &m_value_bin);
        memcpy(v, data, size);
        return *this;
    };

};


/**
 * Background object for async get,
 *  using new BinaryValue object
 */

class GetTask : public WorkTask
{
protected:
    std::string                        m_Key;
    leveldb::ReadOptions              options;

public:
    GetTask(ErlNifEnv *_caller_env,
            ERL_NIF_TERM _caller_ref,
            DbObjectPtr_t & _db_handle,
            ERL_NIF_TERM _key_term,
            leveldb::ReadOptions &_options)
        : WorkTask(_caller_env, _caller_ref, _db_handle),
        options(_options)
        {
            ErlNifBinary key;

            enif_inspect_binary(_caller_env, _key_term, &key);
            m_Key.assign((const char *)key.data, key.size);
        }

    virtual ~GetTask()
    {
    }

protected:
    virtual work_result DoWork()
    {
        ERL_NIF_TERM value_bin;
        BinaryValue value(local_env(), value_bin);
        leveldb::Slice key_slice(m_Key);

        leveldb::Status status = m_DbPtr->m_Db->Get(options, key_slice, &value);

        if(!status.ok())
            return work_result(ATOM_NOT_FOUND);

        return work_result(local_env(), ATOM_OK, value_bin);
    }

};  // class GetTask



/**
 * Background object to open/start an iteration
 */

class IterTask : public WorkTask
{
protected:

    const bool keys_only;
    leveldb::ReadOptions options;

public:
    IterTask(ErlNifEnv *_caller_env,
             ERL_NIF_TERM _caller_ref,
             DbObjectPtr_t & _db_handle,
             const bool _keys_only,
             leveldb::ReadOptions &_options)
        : WorkTask(_caller_env, _caller_ref, _db_handle),
        keys_only(_keys_only), options(_options)
    {}

    virtual ~IterTask()
    {
    }

protected:
    virtual work_result DoWork()
    {
        ItrObject * itr_ptr;
        void * itr_ptr_ptr;

        // NOTE: transfering ownership of options to ItrObject
        itr_ptr_ptr=ItrObject::CreateItrObject(m_DbPtr, keys_only, options);

        // Copy caller_ref to reuse in future iterator_move calls
        itr_ptr=((ItrObjErlang*)itr_ptr_ptr)->m_ItrPtr;
        itr_ptr->itr_ref_env = enif_alloc_env();
        itr_ptr->itr_ref = enif_make_copy(itr_ptr->itr_ref_env, caller_ref());

        ERL_NIF_TERM result = enif_make_resource(local_env(), itr_ptr_ptr);

        // release reference created during CreateItrObject()
        enif_release_resource(itr_ptr_ptr);

        return work_result(local_env(), ATOM_OK, result);
    }

};  // class IterTask


class MoveTask : public WorkTask
{
public:
    typedef enum { FIRST, LAST, NEXT, PREV, SEEK, PREFETCH, PREFETCH_STOP } action_t;

protected:
    ItrObjectPtr_t m_Itr;

public:
    action_t                                       action;
    std::string                                 seek_target;

public:

    // No seek target:
    MoveTask(ErlNifEnv *_caller_env, ERL_NIF_TERM _caller_ref,
             ItrObjectPtr_t & Iter, action_t& _action)
        : WorkTask(NULL, _caller_ref, Iter->m_DbPtr),
        m_Itr(Iter), action(_action)
    {
        // special case construction
        local_env_=NULL;
        enif_self(_caller_env, &local_pid);
    }

    // With seek target:
    MoveTask(ErlNifEnv *_caller_env, ERL_NIF_TERM _caller_ref,
             ItrObjectPtr_t & Iter, action_t& _action,
             std::string& _seek_target)
        : WorkTask(NULL, _caller_ref, Iter->m_DbPtr),
        m_Itr(Iter), action(_action),
        seek_target(_seek_target)
        {
            // special case construction
            local_env_=NULL;
            enif_self(_caller_env, &local_pid);
        }
    virtual ~MoveTask() {};

    virtual ErlNifEnv *local_env();

    virtual void recycle();

protected:
    virtual work_result DoWork();

};  // class MoveTask


/**
 * Background object for async databass close
 */

class CloseTask : public WorkTask
{
protected:

public:

    CloseTask(ErlNifEnv* _owner_env, ERL_NIF_TERM _caller_ref,
              DbObjectPtr_t & _db_handle)
        : WorkTask(_owner_env, _caller_ref, _db_handle)
    {}

    virtual ~CloseTask()
    {
    }

protected:
    virtual work_result DoWork()
    {
        DbObject * db_ptr;

        // get db pointer then clear reference count to it
        db_ptr=m_DbPtr.get();
        m_DbPtr.assign(NULL);

        if (NULL!=db_ptr)
        {
            // set closing flag, this is blocking
            db_ptr->InitiateCloseRequest();

            // db_ptr no longer valid
            db_ptr=NULL;

            return(work_result(ATOM_OK));
        }   // if
        else
        {
            return work_result(local_env(), ATOM_ERROR, ATOM_BADARG);
        }   // else
    }

};  // class CloseTask


/**
 * Background object for async iterator close
 */

class ItrCloseTask : public WorkTask
{
protected:
    ReferencePtr<ItrObject> m_ItrPtr;

public:

    ItrCloseTask(ErlNifEnv* _owner_env, ERL_NIF_TERM _caller_ref,
              ItrObjectPtr_t & _itr_handle)
        : WorkTask(_owner_env, _caller_ref),
        m_ItrPtr(_itr_handle)
    {}

    virtual ~ItrCloseTask()
    {
    }

protected:
    virtual work_result DoWork()
    {
        ItrObject * itr_ptr;

        // get iterator pointer then clear reference count to it
        itr_ptr=m_ItrPtr.get();
        m_ItrPtr.assign(NULL);

        if (NULL!=itr_ptr)
        {
            // set closing flag, this is blocking
            itr_ptr->InitiateCloseRequest();

            // itr_ptr no longer valid
            itr_ptr=NULL;

            return(work_result(ATOM_OK));
        }   // if
        else
        {
            return work_result(local_env(), ATOM_ERROR, ATOM_BADARG);
        }   // else
    }

};  // class ItrCloseTask


/**
 * Background object for async open of a leveldb instance
 */

class DestroyTask : public WorkTask
{
protected:
    std::string         db_name;
    leveldb::Options   *open_options;  // associated with db handle, we don't free it

public:
    DestroyTask(ErlNifEnv* caller_env, ERL_NIF_TERM& _caller_ref,
             const std::string& db_name_, leveldb::Options *open_options_);

    virtual ~DestroyTask() {};

protected:
    virtual work_result DoWork();

private:
    DestroyTask();
    DestroyTask(const DestroyTask &);
    DestroyTask & operator=(const DestroyTask &);

};  // class DestroyTask



} // namespace eleveldb


#endif  // INCL_WORKITEMS_H
