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
#pragma once

#include <stddef.h>

#include <mutex>

#include "kudu/gutil/once.h"
#include "kudu/gutil/port.h"
#include "kudu/util/atomic.h"
#include "kudu/util/status.h"

namespace kudu {

class KuduOnceDynamic;

namespace internal {

// Cheap, single-arg "bound callback" (similar to kudu::Callback) for use
// in KuduOnceDynamic.
template<typename T>
struct MemberFunc {
  KuduOnceDynamic* once;
  T* instance;
  Status (T::*member_func)();
};

template<typename T>
void InitCb(void* arg) {
  MemberFunc<T>* mf = reinterpret_cast<MemberFunc<T>*>(arg);
  mf->once->status_ = (mf->instance->*mf->member_func)();
  if (PREDICT_TRUE(mf->once->status_.ok())) {
    mf->once->set_init_succeeded();
  }
}

} // namespace internal

// More versatile version of GoogleOnceDynamic, including the following:
// - Non-static member functions are registered and run via Init().
// - The first time Init() is called, the registered function is run and the
//   resulting status is stored.
// - Regardless of whether Init() succeeded, the function will cease to run on
//   subsequent calls to Init(), and the stored result will be returned instead.
// - Access to initialization state is safe for concurrent use.
class KuduOnceDynamic {
 public:
  KuduOnceDynamic()
    : init_succeeded_(false) {
  }

  // If the underlying GoogleOnceDynamic has yet to be invoked, invokes the
  // provided member function and stores its return value. Otherwise,
  // returns the stored Status.
  //
  // T: the type of the member passed in.
  template<typename T>
  Status Init(Status (T::*member_func)(), T* instance) {
    internal::MemberFunc<T> mf = { this, instance, member_func };

    // Clang UBSAN doesn't like it when GoogleOnceDynamic handles the cast
    // of the argument:
    //
    //   runtime error: call to function
    //   kudu::cfile::BloomFileReader::InitOnceCb(kudu::cfile::BloomFileReader*)
    //   through pointer to incorrect function type 'void (*)(void *)'
    //
    // So let's do the cast ourselves, to void* here and back in InitCb().
    once_.Init(&internal::InitCb<T>, reinterpret_cast<void*>(&mf));
    return status_;
  }

  // kMemOrderAcquire ensures that loads/stores that come after init_succeeded()
  // aren't reordered to come before it instead. kMemOrderRelease ensures
  // the opposite (i.e. loads/stores before set_init_succeeded() aren't reordered
  // to come after it).
  //
  // Taken together, threads can safely synchronize on init_succeeded_.
  bool init_succeeded() const { return init_succeeded_.Load(kMemOrderAcquire); }

  // Returns the memory usage of this object without the object itself. Should
  // be used when embedded inside another object.
  size_t memory_footprint_excluding_this() const;

  // Returns the memory usage of this object including the object itself.
  // Should be used when allocated on the heap.
  size_t memory_footprint_including_this() const;

 private:
  template<typename T>
  friend void internal::InitCb(void* arg);

  void set_init_succeeded() { init_succeeded_.Store(true, kMemOrderRelease); }

  AtomicBool init_succeeded_;
  GoogleOnceDynamic once_;
  Status status_;
};

// Similar to the KuduOnceDynamic class, but accepts a lambda function.
class KuduOnceLambda {
 public:
  KuduOnceLambda()
    : init_succeeded_(false) {}

  // If the underlying `once_flag` has yet to be invoked, invokes the provided
  // lambda and stores its return value. Otherwise, returns the stored Status.
  template<typename Fn>
  Status Init(Fn fn) {
    std::call_once(once_flag_, [this, fn] {
      status_ = fn();
      if (PREDICT_TRUE(status_.ok())) {
        init_succeeded_.Store(true, kMemOrderRelease);
      }
    });
    return status_;
  }

  // Similar to KuduOnceDynamic, kMemOrderAcquire here and kMemOrderRelease in
  // Init(), taken together, mean that threads can safely synchronize on
  // ini_succeeded_.
  bool init_succeeded() const {
    return init_succeeded_.Load(kMemOrderAcquire);
  }

  // Returns the memory usage of this object without the object itself. Should
  // be used when embedded inside another object.
  size_t memory_footprint_excluding_this() const;

  // Returns the memory usage of this object including the object itself.
  // Should be used when allocated on the heap.
  size_t memory_footprint_including_this() const;

 private:
  AtomicBool init_succeeded_;
  std::once_flag once_flag_;
  Status status_;
};

} // namespace kudu
