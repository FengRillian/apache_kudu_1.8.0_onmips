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

#include "kudu/cfile/bloomfile-test-base.h"

#include <cstdint>
#include <vector>

#include <boost/bind.hpp> // IWYU pragma: keep
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/thread.h"

DEFINE_int32(benchmark_num_threads, 8, "Number of threads to use for the benchmark");

namespace kudu {
namespace cfile {

class MTBloomFileTest : public BloomFileTestBase {
};

TEST_F(MTBloomFileTest, Benchmark) {
  ASSERT_NO_FATAL_FAILURE(WriteTestBloomFile());
  ASSERT_OK(OpenBloomFile());

  std::vector<scoped_refptr<kudu::Thread> > threads;

  for (int i = 0; i < FLAGS_benchmark_num_threads; i++) {
    scoped_refptr<kudu::Thread> new_thread;
    CHECK_OK(Thread::Create("test", strings::Substitute("t$0", i),
                            boost::bind(&BloomFileTestBase::ReadBenchmark, this),
                            &new_thread));
    threads.push_back(new_thread);
  }
  for (scoped_refptr<kudu::Thread>& t : threads) {
    t->Join();
  }
}

} // namespace cfile
} // namespace kudu
