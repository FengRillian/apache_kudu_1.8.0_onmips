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

#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/clock/clock.h"
#include "kudu/clock/hybrid_clock.h"
#include "kudu/clock/logical_clock.h"
#include "kudu/common/timestamp.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/tablet/mvcc.h"
#include "kudu/util/locks.h"
#include "kudu/util/monotime.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

using std::thread;

namespace kudu {
namespace tablet {

using clock::Clock;
using clock::HybridClock;

class MvccTest : public KuduTest {
 public:
  MvccTest()
      : clock_(
          clock::LogicalClock::CreateStartingAt(Timestamp::kInitialTimestamp)) {
  }

  void WaitForSnapshotAtTSThread(MvccManager* mgr, Timestamp ts) {
    MvccSnapshot s;
    CHECK_OK(mgr->WaitForSnapshotWithAllCommitted(ts, &s, MonoTime::Max()));
    CHECK(s.is_clean()) << "verifying postcondition";
    std::lock_guard<simple_spinlock> lock(lock_);
    result_snapshot_.reset(new MvccSnapshot(s));
  }

  bool HasResultSnapshot() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return result_snapshot_ != nullptr;
  }

 protected:
  scoped_refptr<clock::Clock> clock_;

  mutable simple_spinlock lock_;
  gscoped_ptr<MvccSnapshot> result_snapshot_;
};

TEST_F(MvccTest, TestMvccBasic) {
  MvccManager mgr;
  MvccSnapshot snap;

  // Initial state should not have any committed transactions.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 1}]", snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(Timestamp(1)));
  ASSERT_FALSE(snap.IsCommitted(Timestamp(2)));

  // Start timestamp 1
  Timestamp t = clock_->Now();
  ASSERT_EQ(1, t.value());
  mgr.StartTransaction(t);

  // State should still have no committed transactions, since 1 is in-flight.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 1}]", snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(Timestamp(1)));
  ASSERT_FALSE(snap.IsCommitted(Timestamp(2)));

  // Mark timestamp 1 as "applying"
  mgr.StartApplyingTransaction(t);

  // This should not change the set of committed transactions.
  ASSERT_FALSE(snap.IsCommitted(Timestamp(1)));

  // Commit timestamp 1
  mgr.CommitTransaction(t);

  // State should show 0 as committed, 1 as uncommitted.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 1 or (T in {1})}]", snap.ToString());
  ASSERT_TRUE(snap.IsCommitted(Timestamp(1)));
  ASSERT_FALSE(snap.IsCommitted(Timestamp(2)));
}

TEST_F(MvccTest, TestMvccMultipleInFlight) {
  MvccManager mgr;
  MvccSnapshot snap;

  Timestamp t1 = clock_->Now();
  ASSERT_EQ(1, t1.value());
  mgr.StartTransaction(t1);
  Timestamp t2 = clock_->Now();
  ASSERT_EQ(2, t2.value());
  mgr.StartTransaction(t2);

  // State should still have no committed transactions, since both are in-flight.

  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 1}]", snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(t1));
  ASSERT_FALSE(snap.IsCommitted(t2));

  // Commit timestamp 2
  mgr.StartApplyingTransaction(t2);
  mgr.CommitTransaction(t2);

  // State should show 2 as committed, 1 as uncommitted.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed="
            "{T|T < 1 or (T in {2})}]",
            snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(t1));
  ASSERT_TRUE(snap.IsCommitted(t2));

  // Start another transaction. This gets timestamp 3
  Timestamp t3 = clock_->Now();
  ASSERT_EQ(3, t3.value());
  mgr.StartTransaction(t3);

  // State should show 2 as committed, 1 and 4 as uncommitted.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed="
            "{T|T < 1 or (T in {2})}]",
            snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(t1));
  ASSERT_TRUE(snap.IsCommitted(t2));
  ASSERT_FALSE(snap.IsCommitted(t3));

  // Commit 3
  mgr.StartApplyingTransaction(t3);
  mgr.CommitTransaction(t3);

  // 2 and 3 committed
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed="
            "{T|T < 1 or (T in {2,3})}]",
            snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(t1));
  ASSERT_TRUE(snap.IsCommitted(t2));
  ASSERT_TRUE(snap.IsCommitted(t3));

  // Commit 1
  mgr.StartApplyingTransaction(t1);
  mgr.CommitTransaction(t1);

  // All transactions are committed, adjust the safe time.
  mgr.AdjustSafeTime(t3);

  // all committed
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 3 or (T in {3})}]", snap.ToString());
  ASSERT_TRUE(snap.IsCommitted(t1));
  ASSERT_TRUE(snap.IsCommitted(t2));
  ASSERT_TRUE(snap.IsCommitted(t3));
}

TEST_F(MvccTest, TestOutOfOrderTxns) {
  scoped_refptr<Clock> hybrid_clock(new HybridClock());
  ASSERT_OK(hybrid_clock->Init());
  MvccManager mgr;

  // Start a normal non-commit-wait txn.
  Timestamp normal_txn = hybrid_clock->Now();
  mgr.StartTransaction(normal_txn);

  MvccSnapshot s1(mgr);

  // Start a transaction as if it were using commit-wait (i.e. started in future)
  Timestamp cw_txn = hybrid_clock->NowLatest();
  mgr.StartTransaction(cw_txn);

  // Commit the original txn
  mgr.StartApplyingTransaction(normal_txn);
  mgr.CommitTransaction(normal_txn);

  // Start a new txn
  Timestamp normal_txn_2 = hybrid_clock->Now();
  mgr.StartTransaction(normal_txn_2);

  // The old snapshot should not have either txn
  EXPECT_FALSE(s1.IsCommitted(normal_txn));
  EXPECT_FALSE(s1.IsCommitted(normal_txn_2));

  // A new snapshot should have only the first transaction
  MvccSnapshot s2(mgr);
  EXPECT_TRUE(s2.IsCommitted(normal_txn));
  EXPECT_FALSE(s2.IsCommitted(normal_txn_2));

  // Commit the commit-wait one once it is time.
  ASSERT_OK(hybrid_clock->WaitUntilAfter(cw_txn, MonoTime::Max()));
  mgr.StartApplyingTransaction(cw_txn);
  mgr.CommitTransaction(cw_txn);

  // A new snapshot at this point should still think that normal_txn_2 is uncommitted
  MvccSnapshot s3(mgr);
  EXPECT_FALSE(s3.IsCommitted(normal_txn_2));
}

// Tests starting transaction at a point-in-time in the past and committing them while
// adjusting safe time.
TEST_F(MvccTest, TestSafeTimeWithOutOfOrderTxns) {
  MvccManager mgr;

  // Set the clock to some time in the "future".
  ASSERT_OK(clock_->Update(Timestamp(100)));

  // Start a transaction in the "past"
  Timestamp txn_in_the_past(50);
  mgr.StartTransaction(txn_in_the_past);
  mgr.StartApplyingTransaction(txn_in_the_past);

  ASSERT_EQ(Timestamp::kInitialTimestamp, mgr.GetCleanTimestamp());

  // Committing 'txn_in_the_past' should not advance safe time or clean time.
  mgr.CommitTransaction(txn_in_the_past);

  // Now take a snapshot.
  MvccSnapshot snap1;
  mgr.TakeSnapshot(&snap1);

  // Because we did not advance the the safe or clean watermarkd, even though the only
  // in-flight transaction was committed at time 50, a transaction at time 40 should still be
  // considered uncommitted.
  ASSERT_FALSE(snap1.IsCommitted(Timestamp(40)));

  // Now advance the both clean and safe watermarks to the last committed transaction.
  mgr.AdjustSafeTime(Timestamp(50));

  ASSERT_EQ(txn_in_the_past, mgr.GetCleanTimestamp());

  MvccSnapshot snap2;
  mgr.TakeSnapshot(&snap2);

  ASSERT_TRUE(snap2.IsCommitted(Timestamp(40)));
}

TEST_F(MvccTest, TestScopedTransaction) {
  MvccManager mgr;
  MvccSnapshot snap;

  {
    ScopedTransaction t1(&mgr, clock_->Now());
    ScopedTransaction t2(&mgr, clock_->Now());

    ASSERT_EQ(1, t1.timestamp().value());
    ASSERT_EQ(2, t2.timestamp().value());

    t1.StartApplying();
    t1.Commit();

    mgr.TakeSnapshot(&snap);
    ASSERT_TRUE(snap.IsCommitted(t1.timestamp()));
    ASSERT_FALSE(snap.IsCommitted(t2.timestamp()));
  }

  // t2 going out of scope aborts it.
  mgr.TakeSnapshot(&snap);
  ASSERT_TRUE(snap.IsCommitted(Timestamp(1)));
  ASSERT_FALSE(snap.IsCommitted(Timestamp(2)));

  // Test that an applying scoped transaction does not crash if it goes out of
  // scope while the MvccManager is closed.
  mgr.Close();
  {
    ScopedTransaction t(&mgr, clock_->Now());
    NO_FATALS(t.StartApplying());
  }
}

TEST_F(MvccTest, TestPointInTimeSnapshot) {
  MvccSnapshot snap(Timestamp(10));

  ASSERT_TRUE(snap.IsCommitted(Timestamp(1)));
  ASSERT_TRUE(snap.IsCommitted(Timestamp(9)));
  ASSERT_FALSE(snap.IsCommitted(Timestamp(10)));
  ASSERT_FALSE(snap.IsCommitted(Timestamp(11)));
}

TEST_F(MvccTest, TestMayHaveCommittedTransactionsAtOrAfter) {
  MvccSnapshot snap;
  snap.all_committed_before_ = Timestamp(10);
  snap.committed_timestamps_.push_back(11);
  snap.committed_timestamps_.push_back(13);
  snap.none_committed_at_or_after_ = Timestamp(14);

  ASSERT_TRUE(snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(9)));
  ASSERT_TRUE(snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(10)));
  ASSERT_TRUE(snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(12)));
  ASSERT_TRUE(snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(13)));
  ASSERT_FALSE(snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(14)));
  ASSERT_FALSE(snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(15)));

  // Test for "all committed" snapshot
  MvccSnapshot all_committed =
      MvccSnapshot::CreateSnapshotIncludingAllTransactions();
  ASSERT_TRUE(
      all_committed.MayHaveCommittedTransactionsAtOrAfter(Timestamp(1)));
  ASSERT_TRUE(
      all_committed.MayHaveCommittedTransactionsAtOrAfter(Timestamp(12345)));

  // And "none committed" snapshot
  MvccSnapshot none_committed =
      MvccSnapshot::CreateSnapshotIncludingNoTransactions();
  ASSERT_FALSE(
      none_committed.MayHaveCommittedTransactionsAtOrAfter(Timestamp(1)));
  ASSERT_FALSE(
      none_committed.MayHaveCommittedTransactionsAtOrAfter(Timestamp(12345)));

  // Test for a "clean" snapshot
  MvccSnapshot clean_snap(Timestamp(10));
  ASSERT_TRUE(clean_snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(9)));
  ASSERT_FALSE(clean_snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(10)));
}

TEST_F(MvccTest, TestMayHaveUncommittedTransactionsBefore) {
  MvccSnapshot snap;
  snap.all_committed_before_ = Timestamp(10);
  snap.committed_timestamps_.push_back(11);
  snap.committed_timestamps_.push_back(13);
  snap.none_committed_at_or_after_ = Timestamp(14);

  ASSERT_FALSE(snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(9)));
  ASSERT_TRUE(snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(10)));
  ASSERT_TRUE(snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(11)));
  ASSERT_TRUE(snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(13)));
  ASSERT_TRUE(snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(14)));
  ASSERT_TRUE(snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(15)));

  // Test for "all committed" snapshot
  MvccSnapshot all_committed =
      MvccSnapshot::CreateSnapshotIncludingAllTransactions();
  ASSERT_FALSE(
      all_committed.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(1)));
  ASSERT_FALSE(
      all_committed.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(12345)));

  // And "none committed" snapshot
  MvccSnapshot none_committed =
      MvccSnapshot::CreateSnapshotIncludingNoTransactions();
  ASSERT_TRUE(
      none_committed.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(1)));
  ASSERT_TRUE(
      none_committed.MayHaveUncommittedTransactionsAtOrBefore(
          Timestamp(12345)));

  // Test for a "clean" snapshot
  MvccSnapshot clean_snap(Timestamp(10));
  ASSERT_FALSE(clean_snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(9)));
  ASSERT_TRUE(clean_snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(10)));

  // Test for the case where we have a single transaction in flight. Since this is
  // also the earliest transaction, all_committed_before_ is equal to the txn's
  // ts, but when it gets committed we can't advance all_committed_before_ past it
  // because there is no other transaction to advance it to. In this case we should
  // still report that there can't be any uncommitted transactions before.
  MvccSnapshot snap2;
  snap2.all_committed_before_ = Timestamp(10);
  snap2.committed_timestamps_.push_back(10);

  ASSERT_FALSE(snap2.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(10)));
}

TEST_F(MvccTest, TestAreAllTransactionsCommitted) {
  MvccManager mgr;

  // start several transactions and take snapshots along the way
  Timestamp tx1 = clock_->Now();
  mgr.StartTransaction(tx1);
  Timestamp tx2 = clock_->Now();
  mgr.StartTransaction(tx2);
  Timestamp tx3 = clock_->Now();
  mgr.StartTransaction(tx3);
  mgr.AdjustSafeTime(clock_->Now());

  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(1)));
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(2)));
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(3)));

  // commit tx3, should all still report as having as having uncommitted
  // transactions.
  mgr.StartApplyingTransaction(tx3);
  mgr.CommitTransaction(tx3);
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(1)));
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(2)));
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(3)));

  // commit tx1, first snap with in-flights should now report as all committed
  // and remaining snaps as still having uncommitted transactions
  mgr.StartApplyingTransaction(tx1);
  mgr.CommitTransaction(tx1);
  ASSERT_TRUE(mgr.AreAllTransactionsCommitted(Timestamp(1)));
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(2)));
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(3)));

  // Now they should all report as all committed.
  mgr.StartApplyingTransaction(tx2);
  mgr.CommitTransaction(tx2);
  ASSERT_TRUE(mgr.AreAllTransactionsCommitted(Timestamp(1)));
  ASSERT_TRUE(mgr.AreAllTransactionsCommitted(Timestamp(2)));
  ASSERT_TRUE(mgr.AreAllTransactionsCommitted(Timestamp(3)));
}

TEST_F(MvccTest, TestWaitForCleanSnapshot_SnapWithNoInflights) {
  MvccManager mgr;
  Timestamp to_wait_for = clock_->Now();
  mgr.AdjustSafeTime(clock_->Now());
  thread waiting_thread = thread(&MvccTest::WaitForSnapshotAtTSThread, this, &mgr, to_wait_for);

  // join immediately.
  waiting_thread.join();
  ASSERT_TRUE(HasResultSnapshot());
}

TEST_F(MvccTest, TestWaitForCleanSnapshot_SnapBeforeSafeTimeWithInFlights) {
  MvccManager mgr;

  Timestamp tx1 = clock_->Now();
  mgr.StartTransaction(tx1);
  Timestamp tx2 = clock_->Now();
  mgr.StartTransaction(tx2);
  mgr.AdjustSafeTime(tx2);
  Timestamp to_wait_for = clock_->Now();

  // Select a safe time that is after all transactions and after the the timestamp we'll wait for
  // and adjust it on the MvccManager. This will cause "clean time" to move when tx1 and tx2 commit.
  Timestamp safe_time = clock_->Now();
  mgr.AdjustSafeTime(safe_time);

  thread waiting_thread = thread(&MvccTest::WaitForSnapshotAtTSThread, this, &mgr, to_wait_for);

  ASSERT_FALSE(HasResultSnapshot());
  mgr.StartApplyingTransaction(tx1);
  mgr.CommitTransaction(tx1);
  ASSERT_FALSE(HasResultSnapshot());
  mgr.StartApplyingTransaction(tx2);
  mgr.CommitTransaction(tx2);
  waiting_thread.join();
  ASSERT_TRUE(HasResultSnapshot());
}

TEST_F(MvccTest, TestWaitForCleanSnapshot_SnapAfterSafeTimeWithInFlights) {
  MvccManager mgr;
  Timestamp tx1 = clock_->Now();
  mgr.StartTransaction(tx1);
  Timestamp tx2 = clock_->Now();
  mgr.StartTransaction(tx2);
  mgr.AdjustSafeTime(tx2);

  // Wait should return immediately, since we have no transactions "applying"
  // yet.
  ASSERT_OK(mgr.WaitForApplyingTransactionsToCommit());

  mgr.StartApplyingTransaction(tx1);

  Status s;
  thread waiting_thread = thread([&] {
    s = mgr.WaitForApplyingTransactionsToCommit();
  });
  while (mgr.GetNumWaitersForTests() == 0) {
    SleepFor(MonoDelta::FromMilliseconds(5));
  }
  ASSERT_EQ(mgr.GetNumWaitersForTests(), 1);

  // Aborting the other transaction shouldn't affect our waiter.
  mgr.AbortTransaction(tx2);
  ASSERT_EQ(mgr.GetNumWaitersForTests(), 1);

  // Committing our transaction should wake the waiter.
  mgr.CommitTransaction(tx1);
  ASSERT_EQ(mgr.GetNumWaitersForTests(), 0);
  waiting_thread.join();
  ASSERT_OK(s);
}

TEST_F(MvccTest, TestWaitForCleanSnapshot_SnapAtTimestampWithInFlights) {
  MvccManager mgr;

  // Transactions with timestamp 1 through 3
  Timestamp tx1 = clock_->Now();
  mgr.StartTransaction(tx1);
  Timestamp tx2 = clock_->Now();
  mgr.StartTransaction(tx2);
  Timestamp tx3 = clock_->Now();
  mgr.StartTransaction(tx3);

  // Start a thread waiting for transactions with ts <= 2 to commit
  thread waiting_thread = thread(&MvccTest::WaitForSnapshotAtTSThread, this, &mgr, tx2);
  ASSERT_FALSE(HasResultSnapshot());

  // Commit tx 1 - thread should still wait.
  mgr.StartApplyingTransaction(tx1);
  mgr.CommitTransaction(tx1);
  SleepFor(MonoDelta::FromMilliseconds(1));
  ASSERT_FALSE(HasResultSnapshot());

  // Commit tx 3 - thread should still wait.
  mgr.StartApplyingTransaction(tx3);
  mgr.CommitTransaction(tx3);
  SleepFor(MonoDelta::FromMilliseconds(1));
  ASSERT_FALSE(HasResultSnapshot());

  // Commit tx 2 - thread should still wait.
  mgr.StartApplyingTransaction(tx2);
  mgr.CommitTransaction(tx2);
  ASSERT_FALSE(HasResultSnapshot());

  // Advance safe time, thread should continue.
  mgr.AdjustSafeTime(tx3);
  waiting_thread.join();
  ASSERT_TRUE(HasResultSnapshot());
}

TEST_F(MvccTest, TestWaitForApplyingTransactionsToCommit) {
  MvccManager mgr;

  Timestamp tx1 = clock_->Now();
  mgr.StartTransaction(tx1);
  Timestamp tx2 = clock_->Now();
  mgr.StartTransaction(tx2);
  mgr.AdjustSafeTime(tx2);

  // Wait should return immediately, since we have no transactions "applying"
  // yet.
  ASSERT_OK(mgr.WaitForApplyingTransactionsToCommit());

  mgr.StartApplyingTransaction(tx1);

  thread waiting_thread = thread(&MvccManager::WaitForApplyingTransactionsToCommit, &mgr);
  while (mgr.GetNumWaitersForTests() == 0) {
    SleepFor(MonoDelta::FromMilliseconds(5));
  }
  ASSERT_EQ(mgr.GetNumWaitersForTests(), 1);

  // Aborting the other transaction shouldn't affect our waiter.
  mgr.AbortTransaction(tx2);
  ASSERT_EQ(mgr.GetNumWaitersForTests(), 1);

  // Committing our transaction should wake the waiter.
  mgr.CommitTransaction(tx1);
  ASSERT_EQ(mgr.GetNumWaitersForTests(), 0);
  waiting_thread.join();
}

// Test to ensure that after MVCC has been closed, it will not Wait and will
// instead return an error.
TEST_F(MvccTest, TestDontWaitAfterClose) {
  MvccManager mgr;
  Timestamp tx1 = clock_->Now();
  mgr.StartTransaction(tx1);
  mgr.AdjustSafeTime(tx1);
  mgr.StartApplyingTransaction(tx1);

  // Spin up a thread to wait on the applying transaction.
  // Lock the changing status. This is only necessary in this test to read the
  // status from the main thread, showing that, regardless of where, closing
  // MVCC will cause waiters to abort mid-wait.
  Status s;
  simple_spinlock status_lock;
  thread waiting_thread = thread([&] {
    std::lock_guard<simple_spinlock> l(status_lock);
    s = mgr.WaitForApplyingTransactionsToCommit();
  });

  // Wait until the waiter actually gets registered.
  while (mgr.GetNumWaitersForTests() == 0) {
    SleepFor(MonoDelta::FromMilliseconds(5));
  }

  // Set that the mgr is closing. This should cause waiters to abort.
  mgr.Close();
  waiting_thread.join();
  ASSERT_STR_CONTAINS(s.ToString(), "closed");
  ASSERT_TRUE(s.IsAborted());

  // New waiters should abort immediately.
  s = mgr.WaitForApplyingTransactionsToCommit();
  ASSERT_STR_CONTAINS(s.ToString(), "closed");
  ASSERT_TRUE(s.IsAborted());
}

// Test that if we abort a transaction we don't advance the safe time and don't
// add the transaction to the committed set.
TEST_F(MvccTest, TestTxnAbort) {

  MvccManager mgr;

  // Transactions with timestamps 1 through 3
  Timestamp tx1 = clock_->Now();
  mgr.StartTransaction(tx1);
  Timestamp tx2 = clock_->Now();
  mgr.StartTransaction(tx2);
  Timestamp tx3 = clock_->Now();
  mgr.StartTransaction(tx3);
  mgr.AdjustSafeTime(tx3);

  // Now abort tx1, this shouldn't move the clean time and the transaction
  // shouldn't be reported as committed.
  mgr.AbortTransaction(tx1);
  ASSERT_EQ(Timestamp::kInitialTimestamp, mgr.GetCleanTimestamp());
  ASSERT_FALSE(mgr.cur_snap_.IsCommitted(tx1));

  // Committing tx3 shouldn't advance the clean time since it is not the earliest
  // in-flight, but it should advance 'safe_time_' to 3.
  mgr.StartApplyingTransaction(tx3);
  mgr.CommitTransaction(tx3);
  ASSERT_TRUE(mgr.cur_snap_.IsCommitted(tx3));
  ASSERT_EQ(tx3, mgr.safe_time_);

  // Committing tx2 should advance the clean time to 3.
  mgr.StartApplyingTransaction(tx2);
  mgr.CommitTransaction(tx2);
  ASSERT_TRUE(mgr.cur_snap_.IsCommitted(tx2));
  ASSERT_EQ(tx3, mgr.GetCleanTimestamp());
}

// This tests for a bug we were observing, where a clean snapshot would not
// coalesce to the latest timestamp.
TEST_F(MvccTest, TestAutomaticCleanTimeMoveToSafeTimeOnCommit) {

  MvccManager mgr;
  clock_->Update(Timestamp(20));

  mgr.StartTransaction(Timestamp(10));
  mgr.StartTransaction(Timestamp(15));
  mgr.AdjustSafeTime(Timestamp(15));

  mgr.StartApplyingTransaction(Timestamp(15));
  mgr.CommitTransaction(Timestamp(15));

  mgr.StartApplyingTransaction(Timestamp(10));
  mgr.CommitTransaction(Timestamp(10));
  ASSERT_EQ(mgr.cur_snap_.ToString(), "MvccSnapshot[committed={T|T < 15 or (T in {15})}]");
}

// Various death tests which ensure that we can only transition in one of the following
// valid ways:
//
// - Start() -> StartApplying() -> Commit()
// - Start() -> Abort()
//
// Any other transition should fire a CHECK failure.
TEST_F(MvccTest, TestIllegalStateTransitionsCrash) {
  MvccManager mgr;
  MvccSnapshot snap;

  EXPECT_DEATH({
      mgr.StartApplyingTransaction(Timestamp(1));
    }, "Cannot mark timestamp 1 as APPLYING: not in the in-flight map");

  // Depending whether this is a DEBUG or RELEASE build, the error message
  // could be different for this case -- the "future timestamp" check is only
  // run in DEBUG builds.
  EXPECT_DEATH({
      mgr.CommitTransaction(Timestamp(1));
    },
    "Trying to commit a transaction with a future timestamp|"
    "Trying to remove timestamp which isn't in the in-flight set: 1");

  clock_->Update(Timestamp(20));

  EXPECT_DEATH({
      mgr.CommitTransaction(Timestamp(1));
    }, "Trying to remove timestamp which isn't in the in-flight set: 1");

  // Start a transaction, and try committing it without having moved to "Applying"
  // state.
  Timestamp t = clock_->Now();
  mgr.StartTransaction(t);
  EXPECT_DEATH({
      mgr.CommitTransaction(t);
    }, "Trying to commit a transaction which never entered APPLYING state");

  // Aborting should succeed, since we never moved to Applying.
  mgr.AbortTransaction(t);

  // Aborting a second time should fail
  EXPECT_DEATH({
      mgr.AbortTransaction(t);
    }, "Trying to remove timestamp which isn't in the in-flight set: 21");

  // Start a new transaction. This time, mark it as Applying.
  t = clock_->Now();
  mgr.StartTransaction(t);
  mgr.AdjustSafeTime(t);
  mgr.StartApplyingTransaction(t);

  // Can only call StartApplying once.
  EXPECT_DEATH({
      mgr.StartApplyingTransaction(t);
    }, "Cannot mark timestamp 22 as APPLYING: wrong state: 1");

  // Cannot Abort() a transaction once we start applying it.
  EXPECT_DEATH({
      mgr.AbortTransaction(t);
    }, "transaction with timestamp 22 cannot be aborted in state 1");

  // We can commit it successfully.
  mgr.CommitTransaction(t);
}

TEST_F(MvccTest, TestWaitUntilCleanDeadline) {
  MvccManager mgr;

  // Transactions with timestamp 1
  Timestamp tx1 = clock_->Now();
  mgr.StartTransaction(tx1);

  // Wait until the 'tx1' timestamp is clean -- this won't happen because the
  // transaction isn't committed yet.
  MonoTime deadline = MonoTime::Now() + MonoDelta::FromMilliseconds(10);
  MvccSnapshot snap;
  Status s = mgr.WaitForSnapshotWithAllCommitted(tx1, &snap, deadline);
  ASSERT_TRUE(s.IsTimedOut()) << s.ToString();
}

// Test for a bug related to the initialization of the MvccManager without
// any pending transactions, i.e. when there are only calls to AdvanceSafeTime().
// Prior to the fix we would advance safe/clean time but not the
// 'none_committed_at_or_after_' watermark, meaning the latter would become lower
// than safe/clean time. This had the effect on compaction of culling delta files
// even though they shouldn't be culled.
// This test makes sure that watermarks are advanced correctly and that delta
// files are culled correctly.
TEST_F(MvccTest, TestCorrectInitWithNoTxns) {
  MvccManager mgr;

  MvccSnapshot snap;
  mgr.TakeSnapshot(&snap);
  EXPECT_EQ(snap.all_committed_before_, Timestamp::kInitialTimestamp);
  EXPECT_EQ(snap.none_committed_at_or_after_, Timestamp::kInitialTimestamp);
  EXPECT_EQ(snap.committed_timestamps_.size(), 0);

  // Read the clock a few times to advance the timestamp
  for (int i = 0; i < 10; i++) clock_->Now();

  // Advance the safe timestamp.
  Timestamp new_safe_time = clock_->Now();
  mgr.AdjustSafeTime(new_safe_time);

  // Test that the snapshot reports that a timestamp lower than the safe time
  // may have be committed transaction before that timestamp.
  // Conversely, test that the snapshot reports that a timestamp greater
  // than the safe time (and thus greater than none_committed_at_or_after_'
  // as there are no other transactions committed) cannot have any committed
  // transactions after that timestamp.
  MvccSnapshot snap2;
  mgr.TakeSnapshot(&snap2);
  Timestamp before_safe_time(new_safe_time.value() - 1);
  Timestamp after_safe_time(new_safe_time.value() + 1);
  EXPECT_TRUE(snap2.MayHaveCommittedTransactionsAtOrAfter(before_safe_time));
  EXPECT_FALSE(snap2.MayHaveCommittedTransactionsAtOrAfter(after_safe_time));

  EXPECT_EQ(snap2.all_committed_before_, new_safe_time);
  EXPECT_EQ(snap2.none_committed_at_or_after_, new_safe_time);
  EXPECT_EQ(snap2.committed_timestamps_.size(), 0);
}

} // namespace tablet
} // namespace kudu
