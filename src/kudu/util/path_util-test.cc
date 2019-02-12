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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kudu/util/path_util.h"

using std::string;
using std::vector;

namespace kudu {

TEST(TestPathUtil, BaseNameTest) {
  ASSERT_EQ(".", BaseName(""));
  ASSERT_EQ(".", BaseName("."));
  ASSERT_EQ("..", BaseName(".."));
  ASSERT_EQ("/", BaseName("/"));
  ASSERT_EQ("/", BaseName("//"));
  ASSERT_EQ("a", BaseName("a"));
  ASSERT_EQ("ab", BaseName("ab"));
  ASSERT_EQ("ab", BaseName("ab/"));
  ASSERT_EQ("cd", BaseName("ab/cd"));
  ASSERT_EQ("ab", BaseName("/ab"));
  ASSERT_EQ("ab", BaseName("/ab///"));
  ASSERT_EQ("cd", BaseName("/ab/cd"));
}

TEST(TestPathUtil, DirNameTest) {
  ASSERT_EQ(".", DirName(""));
  ASSERT_EQ(".", DirName("."));
  ASSERT_EQ(".", DirName(".."));
  ASSERT_EQ("/", DirName("/"));
#if defined(__linux__)
  // On OS X this test case returns "/", while Linux returns "//". On both
  // platforms dirname(1) returns "/". The difference is unlikely to matter in
  // practice.
  ASSERT_EQ("//", DirName("//"));
#else
  ASSERT_EQ("/", DirName("//"));
#endif // defined(__linux__)
  ASSERT_EQ(".", DirName("a"));
  ASSERT_EQ(".", DirName("ab"));
  ASSERT_EQ(".", DirName("ab/"));
  ASSERT_EQ("ab", DirName("ab/cd"));
  ASSERT_EQ("/", DirName("/ab"));
  ASSERT_EQ("/", DirName("/ab///"));
  ASSERT_EQ("/ab", DirName("/ab/cd"));
}

TEST(TestPathUtil, SplitPathTest) {
  typedef vector<string> Vec;
  ASSERT_EQ(Vec({"/"}), SplitPath("/"));
  ASSERT_EQ(Vec({"/", "a", "b"}), SplitPath("/a/b"));
  ASSERT_EQ(Vec({"/", "a", "b"}), SplitPath("/a/b/"));
  ASSERT_EQ(Vec({"a", "b"}), SplitPath("a/b"));
  ASSERT_EQ(Vec({"."}), SplitPath("."));
  ASSERT_EQ(Vec(), SplitPath(""));
}

} // namespace kudu
