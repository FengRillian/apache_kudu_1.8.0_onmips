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

#include <memory>

#include <boost/bind.hpp> // IWYU pragma: keep
#include <boost/function.hpp>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/rpc/messenger.h"
#include "kudu/rpc/rpc-test-base.h"
#include "kudu/util/countdown_latch.h"
#include "kudu/util/monotime.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/thread.h"

using std::shared_ptr;

namespace kudu {
namespace rpc {

class ReactorTest : public RpcTestBase {
 public:
  ReactorTest()
    : latch_(1) {
  }

  void SetUp() override {
    RpcTestBase::SetUp();
    ASSERT_OK(CreateMessenger("my_messenger", &messenger_, 4));
  }

  void ScheduledTask(const Status& status, const Status& expected_status) {
    CHECK_EQ(expected_status.CodeAsString(), status.CodeAsString());
    latch_.CountDown();
  }

  void ScheduledTaskCheckThread(const Status& status, const Thread* thread) {
    CHECK_OK(status);
    CHECK_EQ(thread, Thread::current_thread());
    latch_.CountDown();
  }

  void ScheduledTaskScheduleAgain(const Status& status) {
    messenger_->ScheduleOnReactor(
        boost::bind(&ReactorTest::ScheduledTaskCheckThread, this, _1,
                    Thread::current_thread()),
        MonoDelta::FromMilliseconds(0));
    latch_.CountDown();
  }

 protected:
  shared_ptr<Messenger> messenger_;
  CountDownLatch latch_;
};

TEST_F(ReactorTest, TestFunctionIsCalled) {
  messenger_->ScheduleOnReactor(
      boost::bind(&ReactorTest::ScheduledTask, this, _1, Status::OK()),
      MonoDelta::FromSeconds(0));
  latch_.Wait();
}

TEST_F(ReactorTest, TestFunctionIsCalledAtTheRightTime) {
  MonoTime before = MonoTime::Now();
  messenger_->ScheduleOnReactor(
      boost::bind(&ReactorTest::ScheduledTask, this, _1, Status::OK()),
      MonoDelta::FromMilliseconds(100));
  latch_.Wait();
  MonoTime after = MonoTime::Now();
  MonoDelta delta = after - before;
  CHECK_GE(delta.ToMilliseconds(), 100);
}

TEST_F(ReactorTest, TestFunctionIsCalledIfReactorShutdown) {
  messenger_->ScheduleOnReactor(
      boost::bind(&ReactorTest::ScheduledTask, this, _1,
                  Status::Aborted("doesn't matter")),
      MonoDelta::FromSeconds(60));
  messenger_->Shutdown();
  latch_.Wait();
}

TEST_F(ReactorTest, TestReschedulesOnSameReactorThread) {
  // Our scheduled task will schedule yet another task.
  latch_.Reset(2);

  messenger_->ScheduleOnReactor(
      boost::bind(&ReactorTest::ScheduledTaskScheduleAgain, this, _1),
      MonoDelta::FromSeconds(0));
  latch_.Wait();
  latch_.Wait();
}

} // namespace rpc
} // namespace kudu
