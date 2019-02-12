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

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest_prod.h>

#include "kudu/consensus/log.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/consensus/raft_consensus.h"
#include "kudu/consensus/time_manager.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/callback.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/tablet/metadata.pb.h"
#include "kudu/tablet/tablet.h"
#include "kudu/tablet/tablet_metadata.h"
#include "kudu/tablet/transaction_order_verifier.h"
#include "kudu/tablet/transactions/transaction.h"
#include "kudu/tablet/transactions/transaction_tracker.h"
#include "kudu/util/locks.h"
#include "kudu/util/metrics.h"
#include "kudu/util/status.h"

namespace kudu {
class MaintenanceManager;
class MaintenanceOp;
class MonoDelta;
class ThreadPool;
class ThreadPoolToken;
template <typename Sig>
class Callback;

namespace consensus {
class ConsensusMetadataManager;
class TransactionStatusPB;
}

namespace clock {
class Clock;
}

namespace log {
class LogAnchorRegistry;
} // namespace log

namespace rpc {
class Messenger;
class ResultTracker;
} // namespace rpc

namespace tablet {
class AlterSchemaTransactionState;
class TabletStatusPB;
class TransactionDriver;
class WriteTransactionState;

// A replica in a tablet consensus configuration, which coordinates writes to tablets.
// Each time Write() is called this class appends a new entry to a replicated
// state machine through a consensus algorithm, which makes sure that other
// peers see the same updates in the same order. In addition to this, this
// class also splits the work and coordinates multi-threaded execution.
class TabletReplica : public RefCountedThreadSafe<TabletReplica>,
                      public consensus::ConsensusRoundHandler {
 public:
  TabletReplica(scoped_refptr<TabletMetadata> meta,
                scoped_refptr<consensus::ConsensusMetadataManager> cmeta_manager,
                consensus::RaftPeerPB local_peer_pb,
                ThreadPool* apply_pool,
                Callback<void(const std::string& reason)> mark_dirty_clbk);

  // Initializes RaftConsensus.
  // This must be called before publishing the instance to other threads.
  // If this fails, the TabletReplica instance remains in a NOT_INITIALIZED
  // state.
  Status Init(ThreadPool* raft_pool);

  // Starts the TabletReplica, making it available for Write()s. If this
  // TabletReplica is part of a consensus configuration this will connect it to other replicas
  // in the consensus configuration.
  Status Start(const consensus::ConsensusBootstrapInfo& bootstrap_info,
               std::shared_ptr<tablet::Tablet> tablet,
               scoped_refptr<clock::Clock> clock,
               std::shared_ptr<rpc::Messenger> messenger,
               scoped_refptr<rpc::ResultTracker> result_tracker,
               scoped_refptr<log::Log> log,
               ThreadPool* prepare_pool);

  // Synchronously transition this replica to STOPPED state from any other
  // state. This also stops RaftConsensus. If a Stop() operation is already in
  // progress, blocks until that operation is complete.
  // See tablet/metadata.proto for a description of legal state transitions.
  void Stop();

  // Synchronously transition this replica to SHUTDOWN state from any other state.
  // See tablet/metadata.proto for a description of legal state transitions.
  //
  // If 'error_' has been set to a non-OK status, the final state will be
  // FAILED instead of SHUTDOWN.
  void Shutdown();

  // Check that the tablet is in a RUNNING state.
  Status CheckRunning() const;

  // Wait until the tablet is in a RUNNING state or if there's a timeout.
  // TODO have a way to wait for any state?
  Status WaitUntilConsensusRunning(const MonoDelta& timeout);

  // Submits a write to a tablet and executes it asynchronously.
  // The caller is expected to build and pass a TransactionContext that points
  // to the RPC WriteRequest, WriteResponse, RpcContext and to the tablet's
  // MvccManager.
  Status SubmitWrite(std::unique_ptr<WriteTransactionState> tx_state);

  // Called by the tablet service to start an alter schema transaction.
  //
  // The transaction contains all the information required to execute the
  // AlterSchema operation and send the response back.
  //
  // If the returned Status is OK, the response to the client will be sent
  // asynchronously. Otherwise the tablet service will have to send the response directly.
  //
  // The AlterSchema operation is taking the tablet component lock in exclusive mode
  // meaning that no other operation on the tablet can be executed while the
  // AlterSchema is in progress.
  Status SubmitAlterSchema(std::unique_ptr<AlterSchemaTransactionState> tx_state);

  void GetTabletStatusPB(TabletStatusPB* status_pb_out) const;

  // Used by consensus to create and start a new ReplicaTransaction.
  virtual Status StartFollowerTransaction(
      const scoped_refptr<consensus::ConsensusRound>& round) override;

  // Used by consensus to notify the tablet replica that a consensus-only round
  // has finished, advancing MVCC safe time as appropriate.
  virtual void FinishConsensusOnlyRound(consensus::ConsensusRound* round) override;

  consensus::RaftConsensus* consensus() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return consensus_.get();
  }

  std::shared_ptr<consensus::RaftConsensus> shared_consensus() const {
    std::lock_guard<simple_spinlock> lock(lock_);
    return consensus_;
  }

  Tablet* tablet() const {
    std::lock_guard<simple_spinlock> lock(lock_);
    return tablet_.get();
  }

  scoped_refptr<consensus::TimeManager> time_manager() const {
    return consensus_->time_manager();
  }

  std::shared_ptr<Tablet> shared_tablet() const {
    std::lock_guard<simple_spinlock> lock(lock_);
    return tablet_;
  }

  const TabletStatePB state() const {
    std::lock_guard<simple_spinlock> lock(lock_);
    return state_;
  }

  std::string StateName() const;

  const TabletDataState data_state() const {
    std::lock_guard<simple_spinlock> lock(lock_);
    return meta_->tablet_data_state();
  }

  // Returns the current Raft configuration.
  const consensus::RaftConfigPB RaftConfig() const;

  // Sets the tablet to a BOOTSTRAPPING state, indicating it is starting up.
  void SetBootstrapping();

  // Set a user-readable status message about the tablet. This may appear on
  // the Web UI, for example.
  void SetStatusMessage(const std::string& status);

  // Retrieve the last human-readable status of this tablet replica.
  std::string last_status() const;

  // Sets the error to the provided one.
  void SetError(const Status& error);

  // Returns the error that occurred, when state is FAILED.
  Status error() const {
    std::lock_guard<simple_spinlock> lock(lock_);
    return error_;
  }

  // Returns a human-readable string indicating the state of the tablet.
  // Typically this looks like "NOT_STARTED", "TABLET_DATA_COPYING",
  // etc. For use in places like the Web UI.
  std::string HumanReadableState() const;

  // Adds list of transactions in-flight at the time of the call to 'out'.
  void GetInFlightTransactions(Transaction::TraceType trace_type,
                               std::vector<consensus::TransactionStatusPB>* out) const;

  // Returns the log indexes to be retained for durability and to catch up peers.
  // Used for selection of log segments to delete during Log GC.
  log::RetentionIndexes GetRetentionIndexes() const;

  // See Log::GetReplaySizeMap(...).
  //
  // Returns a non-ok status if the tablet isn't running.
  Status GetReplaySizeMap(std::map<int64_t, int64_t>* replay_size_map) const;

  // Returns the amount of bytes that would be GC'd if RunLogGC() was called.
  //
  // Returns a non-ok status if the tablet isn't running.
  Status GetGCableDataSize(int64_t* retention_size) const;

  // Return a pointer to the Log.
  // TabletReplica keeps a reference to Log after Init().
  log::Log* log() const {
    return log_.get();
  }

  clock::Clock* clock() {
    return clock_.get();
  }

  const scoped_refptr<log::LogAnchorRegistry>& log_anchor_registry() const {
    return log_anchor_registry_;
  }

  // Returns the tablet_id of the tablet managed by this TabletReplica.
  // Returns the correct tablet_id even if the underlying tablet is not available
  // yet.
  const std::string& tablet_id() const { return tablet_id_; }

  // Convenience method to return the permanent_uuid of this peer.
  std::string permanent_uuid() const { return tablet_->metadata()->fs_manager()->uuid(); }

  Status NewLeaderTransactionDriver(gscoped_ptr<Transaction> transaction,
                                    scoped_refptr<TransactionDriver>* driver);

  Status NewReplicaTransactionDriver(gscoped_ptr<Transaction> transaction,
                                     scoped_refptr<TransactionDriver>* driver);

  // Tells the tablet's log to garbage collect.
  Status RunLogGC();

  // Register the maintenance ops associated with this peer's tablet, also invokes
  // Tablet::RegisterMaintenanceOps().
  void RegisterMaintenanceOps(MaintenanceManager* maint_mgr);

  // Unregister the maintenance ops associated with this replica's tablet.
  void UnregisterMaintenanceOps();

  // Cancels the maintenance ops associated with this replica's tablet.
  // Only to be used in tests.
  void CancelMaintenanceOpsForTests();

  // Stops further I/O on the replica.
  void MakeUnavailable(const Status& error);

  // Return pointer to the transaction tracker for this peer.
  const TransactionTracker* transaction_tracker() const { return &txn_tracker_; }

  const scoped_refptr<TabletMetadata>& tablet_metadata() const {
    return meta_;
  }

  // Marks the tablet as dirty so that it's included in the next heartbeat.
  void MarkTabletDirty(const std::string& reason) {
    mark_dirty_clbk_.Run(reason);
  }

  // Return the total on-disk size of this tablet replica, in bytes.
  size_t OnDiskSize() const;

 private:
  friend class RefCountedThreadSafe<TabletReplica>;
  friend class TabletReplicaTest;
  FRIEND_TEST(TabletReplicaTest, TestMRSAnchorPreventsLogGC);
  FRIEND_TEST(TabletReplicaTest, TestDMSAnchorPreventsLogGC);
  FRIEND_TEST(TabletReplicaTest, TestActiveTransactionPreventsLogGC);

  ~TabletReplica();

  // Wait until the TabletReplica is fully in STOPPED, SHUTDOWN, or FAILED
  // state.
  void WaitUntilStopped();

  std::string LogPrefix() const;
  // Transition to another state. Requires that the caller hold 'lock_' if the
  // object has already published to other threads. See tablet/metadata.proto
  // for state descriptions and legal state transitions.
  void set_state(TabletStatePB new_state);

  const scoped_refptr<TabletMetadata> meta_;
  const scoped_refptr<consensus::ConsensusMetadataManager> cmeta_manager_;

  const std::string tablet_id_;
  const consensus::RaftPeerPB local_peer_pb_;
  scoped_refptr<log::LogAnchorRegistry> log_anchor_registry_; // Assigned in tablet_replica-test

  // Pool that executes apply tasks for transactions. This is a multi-threaded
  // pool, constructor-injected by either the Master (for system tables) or
  // the Tablet server.
  ThreadPool* const apply_pool_;

  // Function to mark this TabletReplica's tablet as dirty in the TSTabletManager.
  //
  // Must be called whenever cluster membership or leadership changes, or when
  // the tablet's schema changes.
  const Callback<void(const std::string& reason)> mark_dirty_clbk_;

  TabletStatePB state_;
  Status error_;
  TransactionTracker txn_tracker_;
  TransactionOrderVerifier txn_order_verifier_;
  scoped_refptr<log::Log> log_;
  std::shared_ptr<Tablet> tablet_;
  std::shared_ptr<rpc::Messenger> messenger_;
  std::shared_ptr<consensus::RaftConsensus> consensus_;
  simple_spinlock prepare_replicate_lock_;

  // Lock protecting state_, last_status_, as well as pointers to collaborating
  // classes such as tablet_, consensus_, and maintenance_ops_.
  mutable simple_spinlock lock_;

  // The human-readable last status of the tablet, displayed on the web page, command line
  // tools, etc.
  std::string last_status_;

  // Lock taken during Init/Shutdown which ensures that only a single thread
  // attempts to perform major lifecycle operations (Init/Shutdown) at once.
  // This must be acquired before acquiring lock_ if they are acquired together.
  // We don't just use lock_ since the lifecycle operations may take a while
  // and we'd like other threads to be able to quickly poll the state_ variable
  // during them in order to reject RPCs, etc.
  mutable simple_spinlock state_change_lock_;

  // Token for serial task submission to the server-wide transaction prepare pool.
  std::unique_ptr<ThreadPoolToken> prepare_pool_token_;

  scoped_refptr<clock::Clock> clock_;

  // List of maintenance operations for the tablet that need information that only the peer
  // can provide.
  std::vector<MaintenanceOp*> maintenance_ops_;

  // The result tracker for writes.
  scoped_refptr<rpc::ResultTracker> result_tracker_;

  FunctionGaugeDetacher metric_detacher_;

  DISALLOW_COPY_AND_ASSIGN(TabletReplica);
};

// A callback to wait for the in-flight transactions to complete and to flush
// the Log when they do.
// Tablet is passed as a raw pointer as this callback is set in TabletMetadata and
// were we to keep the tablet as a shared_ptr a circular dependency would occur:
// callback->tablet->metadata->callback. Since the tablet indirectly owns this
// callback we know that is must still be alive when it fires.
class FlushInflightsToLogCallback : public RefCountedThreadSafe<FlushInflightsToLogCallback> {
 public:
  FlushInflightsToLogCallback(Tablet* tablet,
                              const scoped_refptr<log::Log>& log)
   : tablet_(tablet),
     log_(log) {}

  Status WaitForInflightsAndFlushLog();

 private:
  Tablet* tablet_;
  scoped_refptr<log::Log> log_;
};


}  // namespace tablet
}  // namespace kudu

