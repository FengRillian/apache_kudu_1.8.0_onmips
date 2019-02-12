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

#include "kudu/util/thread.h"

#include <sys/types.h>
#include <unistd.h>

#include <ostream>
#include <string>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/gutil/basictypes.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/util/countdown_latch.h"
#include "kudu/util/env.h"
#include "kudu/util/status.h"
#include "kudu/util/stopwatch.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"
#include "kudu/util/thread_restrictions.h"

using std::string;

namespace kudu {

class ThreadTest : public KuduTest {};

// Join with a thread and emit warnings while waiting to join.
// This has to be manually verified.
TEST_F(ThreadTest, TestJoinAndWarn) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "Skipping test in quick test mode, since this sleeps";
    return;
  }

  scoped_refptr<Thread> holder;
  ASSERT_OK(Thread::Create("test", "sleeper thread", usleep, 1000*1000, &holder));
  ASSERT_OK(ThreadJoiner(holder.get())
                   .warn_after_ms(10)
                   .warn_every_ms(100)
                   .Join());
}

TEST_F(ThreadTest, TestFailedJoin) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "Skipping test in quick test mode, since this sleeps";
    return;
  }

  scoped_refptr<Thread> holder;
  ASSERT_OK(Thread::Create("test", "sleeper thread", usleep, 1000*1000, &holder));
  Status s = ThreadJoiner(holder.get())
    .give_up_after_ms(50)
    .Join();
  ASSERT_STR_CONTAINS(s.ToString(), "Timed out after 50ms joining on sleeper thread");
}

static void TryJoinOnSelf() {
  Status s = ThreadJoiner(Thread::current_thread()).Join();
  // Use CHECK instead of ASSERT because gtest isn't thread-safe.
  CHECK(s.IsInvalidArgument());
}

// Try to join on the thread that is currently running.
TEST_F(ThreadTest, TestJoinOnSelf) {
  scoped_refptr<Thread> holder;
  ASSERT_OK(Thread::Create("test", "test", TryJoinOnSelf, &holder));
  holder->Join();
  // Actual assertion is done by the thread spawned above.
}

TEST_F(ThreadTest, TestDoubleJoinIsNoOp) {
  scoped_refptr<Thread> holder;
  ASSERT_OK(Thread::Create("test", "sleeper thread", usleep, 0, &holder));
  ThreadJoiner joiner(holder.get());
  ASSERT_OK(joiner.Join());
  ASSERT_OK(joiner.Join());
}

TEST_F(ThreadTest, ThreadStartBenchmark) {
  std::vector<scoped_refptr<Thread>> threads(1000);
  LOG_TIMING(INFO, "starting threads") {
    for (auto& t : threads) {
      ASSERT_OK(Thread::Create("test", "TestCallOnExit", usleep, 0, &t));
    }
  }
  LOG_TIMING(INFO, "waiting for all threads to publish TIDs") {
    for (auto& t : threads) {
      t->tid();
    }
  }

  for (auto& t : threads) {
    t->Join();
  }
}

// The following tests only run in debug mode, since thread restrictions are no-ops
// in release builds.
#ifndef NDEBUG
TEST_F(ThreadTest, TestThreadRestrictions_IO) {
  // Default should be to allow IO
  ThreadRestrictions::AssertIOAllowed();

  ThreadRestrictions::SetIOAllowed(false);
  {
    ThreadRestrictions::ScopedAllowIO allow_io;
    ASSERT_TRUE(Env::Default()->FileExists("/"));
  }
  ThreadRestrictions::SetIOAllowed(true);

  // Disallow IO - doing IO should crash the process.
  ASSERT_DEATH({
      ThreadRestrictions::SetIOAllowed(false);
      ignore_result(Env::Default()->FileExists("/"));
    },
    "Function marked as IO-only was called from a thread that disallows IO");
}

TEST_F(ThreadTest, TestThreadRestrictions_Waiting) {
  // Default should be to allow IO
  ThreadRestrictions::AssertWaitAllowed();

  ThreadRestrictions::SetWaitAllowed(false);
  {
    ThreadRestrictions::ScopedAllowWait allow_wait;
    CountDownLatch l(0);
    l.Wait();
  }
  ThreadRestrictions::SetWaitAllowed(true);

  // Disallow waiting - blocking on a latch should crash the process.
  ASSERT_DEATH({
      ThreadRestrictions::SetWaitAllowed(false);
      CountDownLatch l(0);
      l.Wait();
    },
    "Waiting is not allowed to be used on this thread");
}
#endif // NDEBUG

} // namespace kudu
