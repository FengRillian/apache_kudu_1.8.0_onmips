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
#include <iosfwd>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/optional/optional.hpp>
#include <gtest/gtest_prod.h>

#include "kudu/client/shared_ptr.h"
#include "kudu/tools/rebalance_algo.h"
#include "kudu/util/monotime.h"
#include "kudu/util/status.h"

namespace kudu {

namespace client {
class KuduClient;
}

namespace tools {

class Ksck;
struct KsckResults;

// A class implementing logic for Kudu cluster rebalancing.
class Rebalancer {
 public:
  // Configuration parameters for the rebalancer aggregated into a struct.
  struct Config {
    Config(std::vector<std::string> master_addresses = {},
           std::vector<std::string> table_filters = {},
           size_t max_moves_per_server = 5,
           size_t max_staleness_interval_sec = 300,
           int64_t max_run_time_sec = 0,
           bool move_rf1_replicas = false,
           bool output_replica_distribution_details = false);

    // Kudu masters' RPC endpoints.
    std::vector<std::string> master_addresses;

    // Names of tables to balance. If empty, every table and the whole cluster
    // will be balanced.
    std::vector<std::string> table_filters;

    // Maximum number of move operations to run concurrently on one server.
    // An 'operation on a server' means a move operation where either source or
    // destination replica is located on the specified server.
    size_t max_moves_per_server;

    // Maximum duration of the 'staleness' interval, when the rebalancer cannot
    // make any progress in scheduling new moves and no prior scheduled moves
    // are left, even if re-synchronizing against the cluster's state again and
    // again. Such a staleness usually happens in case of a persistent problem
    // with the cluster or when some unexpected concurrent activity is present
    // (such as automatic recovery of failed replicas, etc.).
    size_t max_staleness_interval_sec;

    // Maximum run time, in seconds.
    int64_t max_run_time_sec;

    // Whether to move replicas of tablets with replication factor of one.
    bool move_rf1_replicas;

    // Whether Rebalancer::PrintStats() should output per-table and per-server
    // replica distribution details.
    bool output_replica_distribution_details;
  };

  // Represents a concrete move of a replica from one tablet server to another.
  // Formed logically from a TableReplicaMove by specifying a tablet for the move.
  struct ReplicaMove {
    std::string tablet_uuid;
    std::string ts_uuid_from;
    std::string ts_uuid_to;
  };

  enum class RunStatus {
    UNKNOWN,
    CLUSTER_IS_BALANCED,
    TIMED_OUT,
  };

  // A helper type: key is tablet UUID which corresponds to value.tablet_uuid.
  typedef std::unordered_map<std::string, ReplicaMove> MovesInProgress;

  // Create Rebalancer object with the specified configuration.
  explicit Rebalancer(const Config& config);

  // Print the stats on the cluster balance information into the 'out' stream.
  Status PrintStats(std::ostream& out);

  // Run the rebalancing: start the process and return once the balancing
  // criteria are satisfied or if an error occurs. The number of attempted
  // moves is output into the 'moves_count' parameter (if the parameter is
  // not null). The 'result_status' output parameter cannot be null.
  Status Run(RunStatus* result_status, size_t* moves_count = nullptr);

 private:
  // Helper class to find and schedule next available rebalancing move operation
  // and track already scheduled ones.
  class Runner {
   public:
    // The 'max_moves_per_server' specifies the maximum number of operations
    // per tablet server (both the source and the destination are counted in).
    // The 'deadline' specifies the deadline for the run, 'boost::none'
    // if no timeout is set.
    Runner(size_t max_moves_per_server,
           const boost::optional<MonoTime>& deadline);

    // Initialize instance of Runner so it can run against Kudu cluster with
    // the 'master_addresses' RPC endpoints.
    Status Init(std::vector<std::string> master_addresses);

    // Load information on prescribed replica movement operations. Also,
    // populate helper containers and other auxiliary run-time structures
    // used by ScheduleNextMove(). This method is called with every batch
    // of move operations output by the rebalancing algorithm once previously
    // loaded moves have been scheduled.
    void LoadMoves(std::vector<ReplicaMove> replica_moves);

    // Schedule next replica move.
    bool ScheduleNextMove(bool* has_errors, bool* timed_out);

    // Update statuses and auxiliary information on in-progress replica move
    // operations. The 'timed_out' parameter is set to 'true' if not all
    // in-progress operations were processed by the deadline specified by
    // the 'deadline_' member field. The method returns 'true' if it's necessary
    // to clear the state of the in-progress operations, i.e. 'forget'
    // those, starting from a clean state.
    bool UpdateMovesInProgressStatus(bool* has_errors, bool* timed_out);

    uint32_t moves_count() const {
      return moves_count_;
    }

    const MovesInProgress& scheduled_moves() const {
      return scheduled_moves_;
    }

   private:
    // Given the data in the helper containers, find the index describing
    // the next replica move and output it into the 'op_idx' parameter.
    bool FindNextMove(size_t* op_idx);

    // Update the helper containers once a move operation has been scheduled.
    void UpdateOnMoveScheduled(size_t idx,
                               const std::string& tablet_uuid,
                               const std::string& src_ts_uuid,
                               const std::string& dst_ts_uuid,
                               bool is_success);

    // Auxiliary method used by UpdateOnMoveScheduled() implementation.
    void UpdateOnMoveScheduledImpl(
        size_t idx,
        const std::string& ts_uuid,
        bool is_success,
        std::unordered_map<std::string, std::set<size_t>>* op_indices);

    // Update the helper containers once a scheduled operation is complete
    // (i.e. succeeded or failed).
    void UpdateOnMoveCompleted(const std::string& ts_uuid);

    // Maximum allowed number of move operations per server. For a move
    // operation, a source replica adds +1 at the source server and the target
    // replica adds +1 at the destination server.
    const size_t max_moves_per_server_;

    // Deadline for the activity performed by the Runner class in
    // ScheduleNextMoves() and UpadteMovesInProgressStatus() methods.
    const boost::optional<MonoTime> deadline_;

    // Number of successfully completed replica moves operations.
    uint32_t moves_count_;

    // Kudu cluster RPC end-points.
    std::vector<std::string> master_addresses_;

    // The moves to schedule.
    std::vector<ReplicaMove> replica_moves_;

    // Mapping 'tserver UUID' --> 'indices of move operations having the
    // tserver UUID (i.e. the key) as the source of the move operation'.
    std::unordered_map<std::string, std::set<size_t>> src_op_indices_;

    // Mapping 'tserver UUID' --> 'indices of move operations having the
    // tserver UUID (i.e. the key) as the destination of the move operation'.
    std::unordered_map<std::string, std::set<size_t>> dst_op_indices_;

    // Mapping 'tserver UUID' --> 'scheduled move operations count'.
    std::unordered_map<std::string, int32_t> op_count_per_ts_;

    // Mapping 'scheduled move operations count' --> 'tserver UUID'. That's
    // just reversed 'op_count_per_ts_'. Having count as key helps with finding
    // servers with minimum number of scheduled operations while scheduling
    // replica movement operations (it's necessary to preserve the
    // 'maximum-moves-per-server' constraint while doing so).
    std::multimap<int32_t, std::string> ts_per_op_count_;

    // Information on scheduled replica movement operations; keys are
    // tablet UUIDs, values are ReplicaMove structures.
    MovesInProgress scheduled_moves_;

    // Client object to make queries to Kudu masters for various auxiliary info
    // while scheduling move operations and monitoring their status.
    client::sp::shared_ptr<client::KuduClient> client_;
  };

  friend class KsckResultsToClusterBalanceInfoTest;

  // Convert ksck results into cluster balance information suitable for the
  // input of the high-level rebalancing algorithm. The 'moves_in_progress'
  // parameter contains information on the replica moves which have been
  // scheduled by a caller and still in progress: those are considered
  // as successfully completed and applied to the 'ksck_info' when building
  // ClusterBalanceInfo for the specified 'ksck_info' input. The result
  // cluster balance information is output into the 'cbi' parameter. The 'cbi'
  // output parameter cannot be null.
  Status KsckResultsToClusterBalanceInfo(
      const KsckResults& ksck_info,
      const MovesInProgress& moves_in_progress,
      ClusterBalanceInfo* cbi) const;

  // Get next batch of replica moves from the rebalancing algorithm.
  // Essentially, it runs ksck against the cluster and feeds the data into the
  // rebalancing algorithm along with the information on currently pending
  // replica movement operations. The information returned by the high-level
  // rebalancing algorithm is translated into particular replica movement
  // instructions, which are used to populate the 'replica_moves' parameter
  // (the container is cleared first).
  //
  // The 'moves_in_progress' parameter contains information on pending moves.
  // The results are output into 'replica_moves', which will be empty
  // if no next steps are needed to make the cluster balanced.
  Status GetNextMoves(const MovesInProgress& moves_in_progress,
                      std::vector<ReplicaMove>* replica_moves);

  // Given information from the high-level rebalancing algorithm, find
  // appropriate tablet replicas to move on the specified tablet servers.
  // The set of result UUIDs is output into the 'tablet_ids' container (note:
  // the output container is first cleared). If no suitable replicas are found,
  // 'tablet_ids' will be empty with the result status of Status::OK().
  Status FindReplicas(const TableReplicaMove& move,
                      const KsckResults& ksck_info,
                      std::vector<std::string>* tablet_ids) const;

  // Reset ksck-related fields, preparing for a fresh ksck run.
  Status ResetKsck();

  // Filter out move operations at the tablets which already have operations
  // in progress. The 'replica_moves' cannot be null.
  void FilterMoves(const MovesInProgress& scheduled_moves,
                   std::vector<ReplicaMove>* replica_moves);

  // Configuration for the rebalancer.
  const Config config_;

  // Random device and generator for selecting among multiple choices, when
  // appropriate.
  std::random_device random_device_;
  std::mt19937 random_generator_;

  // An instance of the balancing algorithm.
  TwoDimensionalGreedyAlgo algo_;

  // Auxiliary Ksck object to get information on the cluster.
  std::shared_ptr<Ksck> ksck_;

};

} // namespace tools
} // namespace kudu
