// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
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
// Simple pool/freelist for objects of the same type, typically used
// in local context.
#ifndef KUDU_UTIL_OBJECT_POOL_H
#define KUDU_UTIL_OBJECT_POOL_H

#include <glog/logging.h>
#include <stdint.h>
#include "kudu/gutil/manual_constructor.h"
#include "kudu/gutil/gscoped_ptr.h"

namespace kudu {

template<class T>
class ReturnToPool;

// An object pool allocates and destroys a single class of objects
// off of a free-list.
//
// Upon destruction of the pool, any objects allocated from this pool are
// destroyed, regardless of whether they have been explicitly returned to the
// pool.
//
// This class is similar to the boost::pool::object_pool, except that the boost
// implementation seems to have O(n) deallocation performance and benchmarked
// really poorly.
//
// This class is not thread-safe.
template<typename T>
class ObjectPool {
 public:
  typedef ReturnToPool<T> deleter_type;
  typedef gscoped_ptr<T, deleter_type> scoped_ptr;

  ObjectPool() :
    free_list_head_(NULL),
    alloc_list_head_(NULL),
    deleter_(this) {
  }

  ~ObjectPool() {
    // Delete all objects ever allocated from this pool
    ListNode *node = alloc_list_head_;
    while (node != NULL) {
      ListNode *tmp = node;
      node = node->next_on_alloc_list;
      if (!tmp->is_on_freelist) {
        // Have to run the actual destructor if the user forgot to free it.
        tmp->Destroy();
      }
      delete tmp;
    }
  }

  // Construct a new object instance from the pool.
  T *Construct() {
    base::ManualConstructor<T> *obj = GetObject();
    obj->Init();
    return obj->get();
  }

  template<class Arg1>
  T *Construct(Arg1 arg1) {
    base::ManualConstructor<T> *obj = GetObject();
    obj->Init(arg1);
    return obj->get();
  }

  // Destroy an object, running its destructor and returning it to the
  // free-list.
  void Destroy(T *t) {
    CHECK_NOTNULL(t);
    ListNode *node = static_cast<ListNode *>(
      reinterpret_cast<base::ManualConstructor<T> *>(t));

    node->Destroy();

    DCHECK(!node->is_on_freelist);
    node->is_on_freelist = true;
    node->next_on_free_list = free_list_head_;
    free_list_head_ = node;
  }

  // Create a scoped_ptr wrapper around the given pointer which came from this
  // pool.
  // When the scoped_ptr goes out of scope, the object will get released back
  // to the pool.
  scoped_ptr make_scoped_ptr(T *ptr) {
    return scoped_ptr(ptr, deleter_);
  }

 private:
  class ListNode : base::ManualConstructor<T> {
    friend class ObjectPool<T>;

    ListNode *next_on_free_list;
    ListNode *next_on_alloc_list;

    bool is_on_freelist;
  };


  base::ManualConstructor<T> *GetObject() {
    if (free_list_head_ != NULL) {
      ListNode *tmp = free_list_head_;
      free_list_head_ = tmp->next_on_free_list;
      tmp->next_on_free_list = NULL;
      DCHECK(tmp->is_on_freelist);
      tmp->is_on_freelist = false;

      return static_cast<base::ManualConstructor<T> *>(tmp);
    }
    auto new_node = new ListNode();
    new_node->next_on_free_list = NULL;
    new_node->next_on_alloc_list = alloc_list_head_;
    new_node->is_on_freelist = false;
    alloc_list_head_ = new_node;
    return new_node;
  }

  // Keeps track of free objects in this pool.
  ListNode *free_list_head_;

  // Keeps track of all objects ever allocated by this pool.
  ListNode *alloc_list_head_;

  deleter_type deleter_;
};

// Functor which returns the passed objects to a specific object pool.
// This can be used in conjunction with scoped_ptr to automatically release
// an object back to a pool when it goes out of scope.
template<class T>
class ReturnToPool {
 public:
  explicit ReturnToPool(ObjectPool<T> *pool) :
    pool_(pool) {
  }

  inline void operator()(T *ptr) const {
    pool_->Destroy(ptr);
  }

 private:
  ObjectPool<T> *pool_;
};


} // namespace kudu
#endif
