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

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest_prod.h>

#include "kudu/common/timestamp.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/port.h"
#include "kudu/util/locks.h"
#include "kudu/util/status.h"

namespace kudu {
class CountDownLatch;
class MonoTime;

namespace tablet {
class MvccManager;

// A snapshot of the current MVCC state, which can determine whether
// a transaction ID should be considered visible.
class MvccSnapshot {
 public:
  MvccSnapshot();

  // Create a snapshot with the current state of the given manager
  explicit MvccSnapshot(const MvccManager &manager);

  // Create a snapshot at a specific Timestamp.
  //
  // This snapshot considers all transactions with lower timestamps to
  // be committed, and those with higher timestamps to be uncommitted.
  explicit MvccSnapshot(const Timestamp& timestamp);

  // Create a snapshot which considers all transactions as committed.
  // This is mostly useful in test contexts.
  static MvccSnapshot CreateSnapshotIncludingAllTransactions();

  // Creates a snapshot which considers no transactions committed.
  static MvccSnapshot CreateSnapshotIncludingNoTransactions();

  // Return true if the given transaction ID should be considered committed
  // in this snapshot.
  inline bool IsCommitted(const Timestamp& timestamp) const {
    // Inline the most likely path, in which our watermarks determine
    // whether a transaction is committed.
    if (PREDICT_TRUE(timestamp < all_committed_before_)) {
      return true;
    }
    if (PREDICT_TRUE(timestamp >= none_committed_at_or_after_)) {
      return false;
    }
    // Out-of-line the unlikely case which involves more complex (loopy) code.
    return IsCommittedFallback(timestamp);
  }

  // Returns true if this snapshot may have any committed transactions with ID
  // equal to or higher than the provided 'timestamp'.
  // This is mostly useful to avoid scanning REDO deltas in certain cases.
  // If MayHaveCommittedTransactionsAtOrAfter(delta_stats.min) returns true
  // it means that there might be transactions that need to be applied in the
  // context of this snapshot; otherwise no scanning is necessary.
  bool MayHaveCommittedTransactionsAtOrAfter(const Timestamp& timestamp) const;

  // Returns true if this snapshot may have any uncommitted transactions with ID
  // equal to or lower than the provided 'timestamp'.
  // This is mostly useful to avoid scanning UNDO deltas in certain cases.
  // If MayHaveUncommittedTransactionsAtOrBefore(delta_stats.max) returns false it
  // means that all UNDO delta transactions are committed in the context of this
  // snapshot and no scanning is necessary; otherwise there might be some
  // transactions that need to be undone.
  bool MayHaveUncommittedTransactionsAtOrBefore(const Timestamp& timestamp) const;

  // Return a string representation of the set of committed transactions
  // in this snapshot, suitable for debug printouts.
  std::string ToString() const;

  // Return true if the snapshot is considered 'clean'. A clean snapshot is one
  // which is determined only by a timestamp -- the snapshot considers all
  // transactions with timestamps less than some timestamp to be committed,
  // and all other transactions to be uncommitted.
  bool is_clean() const {
    return committed_timestamps_.empty();
  }

  // Consider the given list of timestamps to be committed in this snapshot,
  // even if they weren't when the snapshot was constructed.
  // This is used in the flush path, where the set of commits going into a
  // flushed file may not be a consistent snapshot from the MVCC point of view,
  // yet we need to construct a scanner that accurately represents that set.
  void AddCommittedTimestamps(const std::vector<Timestamp>& timestamps);

 private:
  friend class MvccManager;
  FRIEND_TEST(MvccTest, TestMayHaveCommittedTransactionsAtOrAfter);
  FRIEND_TEST(MvccTest, TestMayHaveUncommittedTransactionsBefore);
  FRIEND_TEST(MvccTest, TestWaitUntilAllCommitted_SnapAtTimestampWithInFlights);
  FRIEND_TEST(MvccTest, TestCorrectInitWithNoTxns);

  bool IsCommittedFallback(const Timestamp& timestamp) const;

  void AddCommittedTimestamp(Timestamp timestamp);

  // Summary rule:
  //   A transaction T is committed if and only if:
  //      T < all_committed_before_ or
  //   or committed_timestamps_.contains(T)
  //
  // In ASCII form, where 'C' represents a committed transaction,
  // and 'U' represents an uncommitted one:
  //
  //   CCCCCCCCCCCCCCCCCUUUUUCUUUCU
  //                    |    \___\___ committed_timestamps_
  //                    |
  //                    \- all_committed_before_


  // A transaction ID below which all transactions have been committed.
  // For any timestamp X, if X < all_committed_timestamp_, then X is committed.
  Timestamp all_committed_before_;

  // A transaction ID at or beyond which no transactions have been committed.
  // For any timestamp X, if X >= none_committed_after_, then X is uncommitted.
  // This is equivalent to max(committed_timestamps_) + 1, but since
  // that vector is unsorted, we cache it.
  Timestamp none_committed_at_or_after_;

  // The set of transactions higher than all_committed_before_timestamp_ which
  // are committed in this snapshot.
  // It might seem like using an unordered_set<> or a set<> would be faster here,
  // but in practice, this list tends to be stay pretty small, and is only
  // rarely consulted (most data will be culled by 'all_committed_before_'
  // or none_committed_at_or_after_. So, using the compact vector structure fits
  // the whole thing on one or two cache lines, and it ends up going faster.
  std::vector<Timestamp::val_type> committed_timestamps_;

};

// Coordinator of MVCC transactions. Threads wishing to make updates use
// the MvccManager to obtain a unique timestamp, usually through the ScopedTransaction
// class defined below.
//
// MVCC is used to defer updates until commit time, and allow iterators to
// operate on a snapshot which contains only committed transactions.
//
// There are two valid paths for a transaction:
//
// 1) StartTransaction() -> StartApplyingTransaction() -> CommitTransaction()
//   or
// 2) StartTransaction() -> AbortTransaction()
//
// When a transaction is ready to start making changes to in-memory data,
// it should transition to APPLYING state by calling StartApplyingTransaction().
// At this point, the transaction should apply its in-memory operations and
// must commit in a bounded amount of time (i.e it should not wait on external
// input such as an RPC from another host).
//
// NOTE: we do not support "rollback" of in-memory edits. Thus, once we call
// StartApplyingTransaction(), the transaction _must_ commit.
//
// See: docs/design_docs/repeatable-reads.md for more information on some of the concepts in
// this class like "clean" and "safe" time.
class MvccManager {
 public:
  MvccManager();

  // Returns an error if the current snapshot has not been adjusted past its
  // initial state. While in this state, it is unsafe for the MvccManager to
  // serve information about already-applied transactions.
  Status CheckIsSafeTimeInitialized() const;

  // Begins a new transaction, which is assigned the provided timestamp.
  //
  // Requires that 'timestamp' is not committed.
  // Requires that 'timestamp' is greater than 'safe_time'.
  void StartTransaction(Timestamp timestamp);

  // Mark that the transaction with the given timestamp is starting to apply
  // its writes to in-memory stores. This must be called before CommitTransaction().
  // If this is called, then AbortTransaction(timestamp) must never be called.
  void StartApplyingTransaction(Timestamp timestamp);

  // Abort the given transaction.
  //
  // If the transaction is not currently in-flight, this will trigger an
  // assertion error. It is an error to abort the same transaction more
  // than once.
  //
  // This makes sure that the transaction with 'timestamp' is removed from
  // the in-flight set.
  //
  // The transaction must not have been marked as 'APPLYING' by calling
  // StartApplyingTransaction(), or else this logs a FATAL error.
  void AbortTransaction(Timestamp timestamp);

  // Commit the given transaction.
  //
  // If the transaction is not currently in-flight, this will trigger an
  // assertion error. It is an error to commit the same transaction more
  // than once.
  //
  // The transaction must already have been marked as 'APPLYING' by calling
  // StartApplyingTransaction(), or else this logs a FATAL error.
  void CommitTransaction(Timestamp timestamp);

  // Adjusts the safe time so that the MvccManager can trim state, provided
  // 'safe_time' is higher than the current safe time.
  //
  // This must only be called when there is a guarantee that there won't be
  // any more transactions with timestamps equal to or lower than 'safe_time'.
  //
  // TODO(dralves) Until leader leases is implemented this should only be called
  // with the timestamps of consensus committed transactions, not with the safe
  // time received from the leader (which can go back without leader leases).
  void AdjustSafeTime(Timestamp safe_time);

  // Take a snapshot of the current MVCC state, which indicates which
  // transactions have been committed at the time of this call.
  void TakeSnapshot(MvccSnapshot *snapshot) const;

  // Take a snapshot of the MVCC state at 'timestamp' (i.e which includes
  // all transactions which have a lower timestamp)
  //
  // If there are any in-flight transactions at a lower timestamp, waits for
  // them to complete before returning.
  //
  // If 'timestamp' was marked safe before the call to this method (e.g. by TimeManager)
  // then the returned snapshot is repeatable.
  Status WaitForSnapshotWithAllCommitted(Timestamp timestamp,
                                         MvccSnapshot* snapshot,
                                         const MonoTime& deadline) const WARN_UNUSED_RESULT;

  // Wait for all operations that are currently APPLYING to commit.
  //
  // NOTE: this does _not_ guarantee that no transactions are APPLYING upon
  // return -- just that those that were APPLYING at call time are finished
  // upon return.
  //
  // Returns Status::Aborted() if MVCC closed while waiting.
  Status WaitForApplyingTransactionsToCommit() const WARN_UNUSED_RESULT;

  bool AreAllTransactionsCommitted(Timestamp ts) const;

  // Return the number of transactions in flight..
  int CountTransactionsInFlight() const;

  // Returns the earliest possible timestamp for an uncommitted transaction.
  // All timestamps before this one are guaranteed to be committed.
  Timestamp GetCleanTimestamp() const;

  // Return the timestamps of all transactions which are currently 'APPLYING'
  // (i.e. those which have started to apply their operations to in-memory data
  // structures). Other transactions may have reserved their timestamps via
  // StartTransaction() but not yet been applied.
  //
  // These transactions are guaranteed to eventually Commit() -- i.e. they will
  // never Abort().
  void GetApplyingTransactionsTimestamps(std::vector<Timestamp>* timestamps) const;

  // Closes the MVCC manager. New transactions will not start, in-flight
  // transactions will exit early on a best-effort basis, and waiting threads
  // will return Status::Aborted().
  void Close();

  // Returns whether MVCC is open and available to manage transactions.
  bool is_open() const {
    return open_.load();
  }

  ~MvccManager();

 private:
  friend class MvccTest;
  FRIEND_TEST(MvccTest, TestAreAllTransactionsCommitted);
  FRIEND_TEST(MvccTest, TestTxnAbort);
  FRIEND_TEST(MvccTest, TestAutomaticCleanTimeMoveToSafeTimeOnCommit);
  FRIEND_TEST(MvccTest, TestWaitForApplyingTransactionsToCommit);
  FRIEND_TEST(MvccTest, TestWaitForCleanSnapshot_SnapAfterSafeTimeWithInFlights);
  FRIEND_TEST(MvccTest, TestDontWaitAfterClose);

  enum TxnState {
    RESERVED,
    APPLYING
  };

  bool InitTransactionUnlocked(const Timestamp& timestamp);

  // TODO(dralves) ponder merging these since the new ALL_COMMITTED path no longer
  // waits for the clean timestamp.
  enum WaitFor {
    ALL_COMMITTED,
    NONE_APPLYING
  };

  struct WaitingState {
    Timestamp timestamp;
    CountDownLatch* latch;
    WaitFor wait_for;
  };

  // Returns an error if the MVCC manager is closed.
  Status CheckOpen() const;

  // Returns true if all transactions before the given timestamp are committed.
  //
  // If 'ts' is not in the past, it's still possible that new transactions could
  // start with a lower timestamp after this returns.
  bool AreAllTransactionsCommittedUnlocked(Timestamp ts) const;

  // Return true if there is any APPLYING operation with a timestamp
  // less than or equal to 'ts'.
  bool AnyApplyingAtOrBeforeUnlocked(Timestamp ts) const;

  // Waits until all transactions before the given time are committed.
  Status WaitUntil(WaitFor wait_for, Timestamp ts,
                   const MonoTime& deadline) const WARN_UNUSED_RESULT;

  // Return true if the condition that the given waiter is waiting on has
  // been achieved.
  bool IsDoneWaitingUnlocked(const WaitingState& waiter) const;

  // Commits the given transaction.
  // Sets *was_earliest to true if this was the earliest in-flight transaction.
  void CommitTransactionUnlocked(Timestamp timestamp,
                                 bool* was_earliest_in_flight);

  // Remove the timestamp 'ts' from the in-flight map.
  // FATALs if the ts is not in the in-flight map.
  // Returns its state.
  TxnState RemoveInFlightAndGetStateUnlocked(Timestamp ts);

  // Adjusts the clean time, i.e. the timestamp such that all transactions with
  // lower timestamps are committed or aborted, based on which transactions are
  // currently in flight and on what is the latest value of 'safe_time_'.
  void AdjustCleanTime();

  // Advances the earliest in-flight timestamp, based on which transactions are
  // currently in-flight. Usually called when the previous earliest transaction
  // commits or aborts.
  void AdvanceEarliestInFlightTimestamp();

  int GetNumWaitersForTests() const {
    std::lock_guard<simple_spinlock> l(lock_);
    return waiters_.size();
  }

  typedef simple_spinlock LockType;
  mutable LockType lock_;

  MvccSnapshot cur_snap_;

  // The set of timestamps corresponding to currently in-flight transactions.
  typedef std::unordered_map<Timestamp::val_type, TxnState> InFlightMap;
  InFlightMap timestamps_in_flight_;

  // A transaction timestamp below which all transactions are either committed or in-flight,
  // meaning no new transactions will be started with a timestamp that is equal
  // to or lower than this one.
  Timestamp safe_time_;

  // The minimum timestamp in timestamps_in_flight_, or Timestamp::kMax
  // if that set is empty. This is cached in order to avoid having to iterate
  // over timestamps_in_flight_ on every commit.
  Timestamp earliest_in_flight_;

  mutable std::vector<WaitingState*> waiters_;

  std::atomic<bool> open_;

  DISALLOW_COPY_AND_ASSIGN(MvccManager);
};

// A scoped handle to a running transaction.
// When this object goes out of scope, the transaction is automatically
// committed.
class ScopedTransaction {
 public:
  // Create a new transaction from the given MvccManager.
  //
  // When this transaction is committed it will use MvccManager::CommitTransaction().
  ScopedTransaction(MvccManager* manager, Timestamp timestamp);

  // Commit the transaction referenced by this scoped object, if it hasn't
  // already been committed.
  ~ScopedTransaction();

  Timestamp timestamp() const {
    return timestamp_;
  }

  // Mark that this transaction is about to begin applying its modifications to
  // in-memory stores.
  //
  // This must be called before Commit(). Abort() may not be called after this
  // method.
  void StartApplying();

  // Commit the in-flight transaction.
  //
  // Requires that StartApplying() has been called.
  void Commit();

  // Abort the in-flight transaction.
  //
  // Requires that StartApplying() has NOT been called.
  void Abort();

 private:
  bool done_;
  MvccManager * const manager_;
  const Timestamp timestamp_;

  DISALLOW_COPY_AND_ASSIGN(ScopedTransaction);
};


} // namespace tablet
} // namespace kudu
