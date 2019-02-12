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

#include <ostream>
#include <string>
#include <type_traits>
#include <utility>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/gutil/bind.h"
#include "kudu/gutil/bind_helpers.h"
#include "kudu/gutil/callback.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/ref_counted.h"

namespace kudu {

using std::string;

static int Return5() {
  return 5;
}

TEST(CallbackBindTest, TestFreeFunction) {
  Callback<int(void)> func_cb = Bind(&Return5);
  ASSERT_EQ(5, func_cb.Run());
}

class Ref : public RefCountedThreadSafe<Ref> {
 public:
  int Foo() { return 3; }
};

// Simple class that helps with verifying ref counting.
// Not thread-safe.
struct RefCountable {
  RefCountable()
      : refs(0) {
  }
  void AddRef() const {
    refs++;
  }
  void Release() const {
    refs--;
  }
  void Print() const {
    LOG(INFO) << "Hello. Refs: " << refs;
  }

  mutable int refs;
  DISALLOW_COPY_AND_ASSIGN(RefCountable);
};

TEST(CallbackBindTest, TestClassMethod) {
  scoped_refptr<Ref> ref = new Ref();
  Callback<int(void)> ref_cb = Bind(&Ref::Foo, ref);
  ref = nullptr;
  ASSERT_EQ(3, ref_cb.Run());
}

int ReturnI(int i, const char* str) {
  return i;
}

TEST(CallbackBindTest, TestPartialBind) {
  Callback<int(const char*)> cb = Bind(&ReturnI, 23);
  ASSERT_EQ(23, cb.Run("hello world"));
}

char IncrementChar(gscoped_ptr<char> in) {
  return *in + 1;
}

TEST(CallbackBindTest, TestCallScopedPtrArg) {
  // Calling a function with a gscoped_ptr argument is just like any other
  // function which takes gscoped_ptr:
  gscoped_ptr<char> foo(new char('x'));
  Callback<char(gscoped_ptr<char>)> cb = Bind(&IncrementChar);
  ASSERT_EQ('y', cb.Run(std::move(foo)));
}

TEST(CallbackBindTest, TestBindScopedPtrArg) {
  // Binding a function with a gscoped_ptr argument requires using Passed()
  gscoped_ptr<char> foo(new char('x'));
  Callback<char(void)> cb = Bind(&IncrementChar, Passed(&foo));
  ASSERT_EQ('y', cb.Run());
}

// Test that the ref counting functionality works.
TEST(CallbackBindTest, TestRefCounting) {
  RefCountable countable;
  {
    ASSERT_EQ(0, countable.refs);
    Closure cb = Bind(&RefCountable::Print, &countable);
    ASSERT_EQ(1, countable.refs);
    cb.Run();
    ASSERT_EQ(1, countable.refs);
  }
  ASSERT_EQ(0, countable.refs);
}

} // namespace kudu
