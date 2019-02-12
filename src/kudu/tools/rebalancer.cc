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

#include "kudu/tools/rebalancer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <glog/logging.h>

#include "kudu/client/client.h"
#include "kudu/gutil/basictypes.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/tools/ksck.h"
#include "kudu/tools/ksck_remote.h"
#include "kudu/tools/ksck_results.h"
#include "kudu/tools/rebalance_algo.h"
#include "kudu/tools/tool_action_common.h"
#include "kudu/tools/tool_replica_util.h"
#include "kudu/util/monotime.h"
#include "kudu/util/status.h"

using kudu::client::KuduClient;
using kudu::client::KuduClientBuilder;
using std::accumulate;
using std::endl;
using std::inserter;
using std::ostream;
using std::map;
using std::multimap;
using std::numeric_limits;
using std::pair;
using std::set_difference;
using std::set;
using std::shared_ptr;
using std::sort;
using std::string;
using std::to_string;
using std::unordered_map;
using std::unordered_set;
using std::vector;
using strings::Substitute;

namespace kudu {
namespace tools {

Rebalancer::Config::Config(
    std::vector<std::string> master_addresses,
    std::vector<std::string> table_filters,
    size_t max_moves_per_server,
    size_t max_staleness_interval_sec,
    int64_t max_run_time_sec,
    bool move_rf1_replicas,
    bool output_replica_distribution_details)
        : master_addresses(std::move(master_addresses)),
          table_filters(std::move(table_filters)),
          max_moves_per_server(max_moves_per_server),
          max_staleness_interval_sec(max_staleness_interval_sec),
          max_run_time_sec(max_run_time_sec),
          move_rf1_replicas(move_rf1_replicas),
          output_replica_distribution_details(output_replica_distribution_details) {
  DCHECK_GE(max_moves_per_server, 0);
}

Rebalancer::Rebalancer(const Config& config)
    : config_(config),
      random_device_(),
      random_generator_(random_device_()) {
}

Status Rebalancer::PrintStats(std::ostream& out) {
  // First, report on the current balance state of the cluster.
  RETURN_NOT_OK(ResetKsck());
  ignore_result(ksck_->Run());
  const KsckResults& results = ksck_->results();

  ClusterBalanceInfo cbi;
  RETURN_NOT_OK(KsckResultsToClusterBalanceInfo(results, MovesInProgress(), &cbi));

  // Per-server replica distribution stats.
  {
    out << "Per-server replica distribution summary:" << endl;
    DataTable summary({"Statistic", "Value"});

    const auto& servers_load_info = cbi.servers_by_total_replica_count;
    if (servers_load_info.empty()) {
      summary.AddRow({ "N/A", "N/A" });
    } else {
      const int64_t total_replica_count = accumulate(
          servers_load_info.begin(), servers_load_info.end(), 0L,
          [](int64_t sum, const pair<int32_t, string>& elem) {
            return sum + elem.first;
          });

      const auto min_replica_count = servers_load_info.begin()->first;
      const auto max_replica_count = servers_load_info.rbegin()->first;
      const double avg_replica_count =
          1.0 * total_replica_count / servers_load_info.size();

      summary.AddRow({ "Minimum Replica Count", to_string(min_replica_count) });
      summary.AddRow({ "Maximum Replica Count", to_string(max_replica_count) });
      summary.AddRow({ "Average Replica Count", to_string(avg_replica_count) });
    }
    RETURN_NOT_OK(summary.PrintTo(out));
    out << endl;

    if (config_.output_replica_distribution_details) {
      const auto& tserver_summaries = results.tserver_summaries;
      unordered_map<string, string> tserver_endpoints;
      for (const auto& summary : tserver_summaries) {
        tserver_endpoints.emplace(summary.uuid, summary.address);
      }

      out << "Per-server replica distribution details:" << endl;
      DataTable servers_info({ "UUID", "Address", "Replica Count" });
      for (const auto& elem : servers_load_info) {
        const auto& id = elem.second;
        servers_info.AddRow({ id, tserver_endpoints[id], to_string(elem.first) });
      }
      RETURN_NOT_OK(servers_info.PrintTo(out));
      out << endl;
    }
  }

  // Per-table replica distribution stats.
  {
    out << "Per-table replica distribution summary:" << endl;
    DataTable summary({ "Replica Skew", "Value" });
    const auto& table_skew_info = cbi.table_info_by_skew;
    if (table_skew_info.empty()) {
      summary.AddRow({ "N/A", "N/A" });
    } else {
      const auto min_table_skew = table_skew_info.begin()->first;
      const auto max_table_skew = table_skew_info.rbegin()->first;
      const int64_t sum_table_skew = accumulate(
          table_skew_info.begin(), table_skew_info.end(), 0L,
          [](int64_t sum, const pair<int32_t, TableBalanceInfo>& elem) {
            return sum + elem.first;
          });
      double avg_table_skew = 1.0 * sum_table_skew / table_skew_info.size();

      summary.AddRow({ "Minimum", to_string(min_table_skew) });
      summary.AddRow({ "Maximum", to_string(max_table_skew) });
      summary.AddRow({ "Average", to_string(avg_table_skew) });
    }
    RETURN_NOT_OK(summary.PrintTo(out));
    out << endl;

    if (config_.output_replica_distribution_details) {
      const auto& table_summaries = results.table_summaries;
      unordered_map<string, const KsckTableSummary*> table_info;
      for (const auto& summary : table_summaries) {
        table_info.emplace(summary.id, &summary);
      }
      out << "Per-table replica distribution details:" << endl;
      DataTable skew(
          { "Table Id", "Replica Count", "Replica Skew", "Table Name" });
      for (const auto& elem : table_skew_info) {
        const auto& table_id = elem.second.table_id;
        const auto it = table_info.find(table_id);
        const auto* table_summary =
            (it == table_info.end()) ? nullptr : it->second;
        const auto& table_name = table_summary ? table_summary->name : "";
        const auto total_replica_count = table_summary
            ? table_summary->replication_factor * table_summary->TotalTablets()
            : 0;
        skew.AddRow({ table_id,
                      to_string(total_replica_count),
                      to_string(elem.first),
                      table_name });
      }
      RETURN_NOT_OK(skew.PrintTo(out));
      out << endl;
    }
  }

  return Status::OK();
}

Status Rebalancer::Run(RunStatus* result_status, size_t* moves_count) {
  DCHECK(result_status);
  *result_status = RunStatus::UNKNOWN;

  boost::optional<MonoTime> deadline;
  if (config_.max_run_time_sec != 0) {
    deadline = MonoTime::Now() + MonoDelta::FromSeconds(config_.max_run_time_sec);
  }

  Runner runner(config_.max_moves_per_server, deadline);
  RETURN_NOT_OK(runner.Init(config_.master_addresses));

  const MonoDelta max_staleness_delta =
      MonoDelta::FromSeconds(config_.max_staleness_interval_sec);
  MonoTime staleness_start = MonoTime::Now();
  bool is_timed_out = false;
  bool resync_state = false;
  while (!is_timed_out) {
    if (resync_state) {
      resync_state = false;
      MonoDelta staleness_delta = MonoTime::Now() - staleness_start;
      if (staleness_delta > max_staleness_delta) {
        LOG(INFO) << Substitute("detected a staleness period of $0", staleness_delta.ToString());
        return Status::Incomplete(Substitute(
            "stalled with no progress for more than $0 seconds, aborting",
            max_staleness_delta.ToString()));
      }
      // The actual re-synchronization happens during GetNextMoves() below:
      // updated info is collected from the cluster and fed into the algorithm.
      LOG(INFO) << "re-synchronizing cluster state";
    }

    {
      vector<Rebalancer::ReplicaMove> replica_moves;
      RETURN_NOT_OK(GetNextMoves(runner.scheduled_moves(), &replica_moves));
      if (replica_moves.empty() && runner.scheduled_moves().empty()) {
        // No moves are left: done!
        break;
      }

      // Filter out moves for tablets which already have operations in progress.
      FilterMoves(runner.scheduled_moves(), &replica_moves);
      runner.LoadMoves(std::move(replica_moves));
    }

    auto has_errors = false;
    while (!is_timed_out) {
      auto is_scheduled = runner.ScheduleNextMove(&has_errors, &is_timed_out);
      resync_state |= has_errors;
      if (resync_state || is_timed_out) {
        break;
      }
      if (is_scheduled) {
        // Reset the start of the staleness interval: there was some progress
        // in scheduling new move operations.
        staleness_start = MonoTime::Now();

        // Continue scheduling available move operations while there is enough
        // capacity, i.e. until number of pending move operations on every
        // involved tablet server reaches max_moves_per_server. Once no more
        // operations can be scheduled, it's time to check for their status.
        continue;
      }

      // Poll for the status of pending operations. If some of the in-flight
      // operations are complete, it might be possible to schedule new ones
      // by calling Runner::ScheduleNextMove().
      auto has_updates = runner.UpdateMovesInProgressStatus(&has_errors,
                                                            &is_timed_out);
      if (has_updates) {
        // Reset the start of the staleness interval: there was some updates
        // on the status of scheduled move operations.
        staleness_start = MonoTime::Now();
      }
      resync_state |= has_errors;
      if (resync_state || is_timed_out || !has_updates) {
        // If there were errors while trying to get the statuses of pending
        // operations it's necessary to re-synchronize the state of the cluster:
        // most likely something has changed, so it's better to get a new set
        // of planned moves.
        break;
      }

      // Sleep a bit before going next cycle of status polling.
      SleepFor(MonoDelta::FromMilliseconds(200));
    }
  }

  *result_status = is_timed_out ? RunStatus::TIMED_OUT
                                : RunStatus::CLUSTER_IS_BALANCED;
  if (moves_count) {
    *moves_count = runner.moves_count();
  }

  return Status::OK();
}

// Transform the information on the cluster returned by ksck into
// ClusterBalanceInfo that could be consumed by the rebalancing algorithm,
// taking into account pending replica movement operations. The pending
// operations are evaluated against the state of the cluster in accordance with
// the ksck results, and if the replica movement operations are still in
// progress, then they are interpreted as successfully completed. The idea is to
// prevent the algorithm outputting the same moves again while some of the
// moves recommended at prior steps are still in progress.
Status Rebalancer::KsckResultsToClusterBalanceInfo(
    const KsckResults& ksck_info,
    const MovesInProgress& pending_moves,
    ClusterBalanceInfo* cbi) const {
  DCHECK(cbi);

  // tserver UUID --> total replica count of all table's tablets at the server
  typedef unordered_map<string, int32_t> TableReplicasAtServer;

  // The result table balance information to build.
  ClusterBalanceInfo balance_info;

  unordered_map<string, int32_t> tserver_replicas_count;
  unordered_map<string, TableReplicasAtServer> table_replicas_info;

  // Build a set of tables with RF=1 (single replica tables).
  unordered_set<string> rf1_tables;
  if (!config_.move_rf1_replicas) {
    for (const auto& s : ksck_info.table_summaries) {
      if (s.replication_factor == 1) {
        rf1_tables.emplace(s.id);
      }
    }
  }

  for (const auto& s : ksck_info.tserver_summaries) {
    if (s.health != KsckServerHealth::HEALTHY) {
      LOG(INFO) << Substitute("skipping tablet server $0 ($1) because of its "
                              "non-HEALTHY status ($2)",
                              s.uuid, s.address,
                              ServerHealthToString(s.health));
      continue;
    }
    tserver_replicas_count.emplace(s.uuid, 0);
  }

  for (const auto& tablet : ksck_info.tablet_summaries) {
    if (!config_.move_rf1_replicas) {
      if (rf1_tables.find(tablet.table_id) != rf1_tables.end()) {
        LOG(INFO) << Substitute("tablet $0 of table '$0' ($1) has single replica, skipping",
                                tablet.id, tablet.table_name, tablet.table_id);
        continue;
      }
    }

    // Check if it's one of the tablets which are currently being rebalanced.
    // If so, interpret the move as successfully completed, updating the
    // replica counts correspondingly.
    const auto it_pending_moves = pending_moves.find(tablet.id);

    for (const auto& ri : tablet.replicas) {
      // Increment total count of replicas at the tablet server.
      auto it = tserver_replicas_count.find(ri.ts_uuid);
      if (it == tserver_replicas_count.end()) {
        string msg = Substitute("skipping replica at tserver $0", ri.ts_uuid);
        if (ri.ts_address) {
          msg += " (" + *ri.ts_address + ")";
        }
        msg += " since it's not reported among known tservers";
        LOG(INFO) << msg;
        continue;
      }
      bool do_count_replica = true;
      if (it_pending_moves != pending_moves.end() &&
          tablet.result == KsckCheckResult::RECOVERING) {
        const auto& move_info = it_pending_moves->second;
        bool is_target_replica_present = false;
        // Verify that the target replica is present in the config.
        for (const auto& tr : tablet.replicas) {
          if (tr.ts_uuid == move_info.ts_uuid_to) {
            is_target_replica_present = true;
            break;
          }
        }
        if (move_info.ts_uuid_from == ri.ts_uuid && is_target_replica_present) {
          // It seems both the source and the destination replicas of the
          // scheduled replica movement operation are still in the config.
          // That's a sign that the move operation hasn't yet completed.
          // As explained above, let's interpret the move as successfully
          // completed, so the source replica should not be counted in.
          do_count_replica = false;
        }
      }
      if (do_count_replica) {
        it->second++;
      }

      auto table_ins = table_replicas_info.emplace(
          tablet.table_id, TableReplicasAtServer());
      TableReplicasAtServer& replicas_at_server = table_ins.first->second;

      auto replicas_ins = replicas_at_server.emplace(ri.ts_uuid, 0);
      if (do_count_replica) {
        replicas_ins.first->second++;
      }
    }
  }

  // Check for the consistency of information derived from the ksck report.
  for (const auto& elem : tserver_replicas_count) {
    const auto& ts_uuid = elem.first;
    int32_t count_by_table_info = 0;
    for (auto& e : table_replicas_info) {
      count_by_table_info += e.second[ts_uuid];
    }
    if (elem.second != count_by_table_info) {
      return Status::Corruption("inconsistent cluster state returned by ksck");
    }
  }

  // Populate ClusterBalanceInfo::servers_by_total_replica_count
  auto& servers_by_count = balance_info.servers_by_total_replica_count;
  for (const auto& elem : tserver_replicas_count) {
    servers_by_count.emplace(elem.second, elem.first);
  }

  // Populate ClusterBalanceInfo::table_info_by_skew
  auto& table_info_by_skew = balance_info.table_info_by_skew;
  for (const auto& elem : table_replicas_info) {
    const auto& table_id = elem.first;
    int32_t max_count = numeric_limits<int32_t>::min();
    int32_t min_count = numeric_limits<int32_t>::max();
    TableBalanceInfo tbi;
    tbi.table_id = table_id;
    for (const auto& e : elem.second) {
      const auto& ts_uuid = e.first;
      const auto replica_count = e.second;
      tbi.servers_by_replica_count.emplace(replica_count, ts_uuid);
      max_count = std::max(replica_count, max_count);
      min_count = std::min(replica_count, min_count);
    }
    table_info_by_skew.emplace(max_count - min_count, std::move(tbi));
  }
  *cbi = std::move(balance_info);

  return Status::OK();
}

// Run one step of the rebalancer. Due to the inherent restrictions of the
// rebalancing engine, no more than one replica per tablet is moved during
// one step of the rebalancing.
Status Rebalancer::GetNextMoves(const MovesInProgress& pending_moves,
                                vector<ReplicaMove>* replica_moves) {
  RETURN_NOT_OK(ResetKsck());
  ignore_result(ksck_->Run());
  const auto& ksck_info = ksck_->results();

  // For simplicity, allow to run the rebalancing only when all tablet servers
  // are in good shape. Otherwise, the rebalancing might interfere with the
  // automatic re-replication or get unexpected errors while moving replicas.
  for (const auto& s : ksck_info.tserver_summaries) {
    if (s.health != KsckServerHealth::HEALTHY) {
      return Status::IllegalState(
          Substitute("tablet server $0 ($1): unacceptable health status $2",
                     s.uuid, s.address, ServerHealthToString(s.health)));
    }
  }

  // The number of operations to output by the algorithm. Those will be
  // translated into concrete tablet replica movement operations, the output of
  // this method.
  const size_t max_moves = config_.max_moves_per_server *
      ksck_info.tserver_summaries.size() * 5;

  replica_moves->clear();
  vector<TableReplicaMove> moves;
  {
    ClusterBalanceInfo cbi;
    RETURN_NOT_OK(KsckResultsToClusterBalanceInfo(ksck_info, pending_moves, &cbi));
    RETURN_NOT_OK(algo_.GetNextMoves(std::move(cbi), max_moves, &moves));
  }
  if (moves.empty()) {
    // No suitable moves were found: the cluster described by 'cbi' is balanced,
    // assuming the pending moves, if any, will succeed.
    return Status::OK();
  }
  unordered_set<string> tablets_in_move;
  std::transform(pending_moves.begin(), pending_moves.end(),
                 inserter(tablets_in_move, tablets_in_move.begin()),
                 [](const MovesInProgress::value_type& elem) {
                   return elem.first;
                 });
  for (const auto& move : moves) {
    vector<string> tablet_ids;
    RETURN_NOT_OK(FindReplicas(move, ksck_info, &tablet_ids));
    // Shuffle the set of the tablet identifiers: that's to achieve even spread
    // of moves across tables with the same skew.
    std::shuffle(tablet_ids.begin(), tablet_ids.end(), random_generator_);
    string move_tablet_id;
    for (const auto& tablet_id : tablet_ids) {
      if (tablets_in_move.find(tablet_id) == tablets_in_move.end()) {
        // For now, choose the very first tablet that does not have replicas
        // in move. Later on, additional logic might be added to find
        // the best candidate.
        move_tablet_id = tablet_id;
        break;
      }
    }
    if (move_tablet_id.empty()) {
      LOG(WARNING) << Substitute(
          "table $0: could not find any suitable replica to move "
          "from server $1 to server $2", move.table_id, move.from, move.to);
      continue;
    }
    ReplicaMove info;
    info.tablet_uuid = move_tablet_id;
    info.ts_uuid_from = move.from;
    info.ts_uuid_to = move.to;
    replica_moves->emplace_back(std::move(info));
    // Mark the tablet as 'has a replica in move'.
    tablets_in_move.emplace(move_tablet_id);
  }

  return Status::OK();
}

// Given high-level description of moves, find tablets with replicas at the
// corresponding tablet servers to satisfy those high-level descriptions.
// The idea is to find all tablets of the specified table that would have a
// replica at the source server, but would not have a replica at the destination
// server. That is to satisfy the restriction of having no more than one replica
// of the same tablet per server.
//
// An additional constraint: it's better not to move leader replicas, if
// possible. If a client has a write operation in progress, moving leader
// replicas of affected tablets would make the client to re-resolve new leaders
// and retry the operations. Moving leader replicas is used as last resort
// when no other candidates are left.
Status Rebalancer::FindReplicas(const TableReplicaMove& move,
                                const KsckResults& ksck_info,
                                vector<string>* tablet_ids) const {
  const auto& table_id = move.table_id;

  // Tablet ids of replicas on the source tserver that are non-leaders.
  vector<string> tablet_uuids_src;
  // Tablet ids of replicas on the source tserver that are leaders.
  vector<string> tablet_uuids_src_leaders;
  // UUIDs of tablets of the selected table at the destination tserver.
  vector<string> tablet_uuids_dst;

  for (const auto& tablet_summary : ksck_info.tablet_summaries) {
    if (tablet_summary.table_id != table_id) {
      continue;
    }
    if (tablet_summary.result != KsckCheckResult::HEALTHY) {
      VLOG(1) << Substitute("table $0: not considering replicas of tablet $1 "
                            "as candidates for movement since the tablet's "
                            "status is '$2'",
                            table_id, tablet_summary.id,
                            KsckCheckResultToString(tablet_summary.result));
      continue;
    }
    for (const auto& replica_summary : tablet_summary.replicas) {
      if (replica_summary.ts_uuid != move.from &&
          replica_summary.ts_uuid != move.to) {
        continue;
      }
      if (!replica_summary.ts_healthy) {
        VLOG(1) << Substitute("table $0: not considering replica movement "
                              "from $1 to $2 since server $3 is not healthy",
                              table_id,
                              move.from, move.to, replica_summary.ts_uuid);
        continue;
      }
      if (replica_summary.ts_uuid == move.from) {
        if (replica_summary.is_leader) {
          tablet_uuids_src_leaders.emplace_back(tablet_summary.id);
        } else {
          tablet_uuids_src.emplace_back(tablet_summary.id);
        }
      } else {
        DCHECK_EQ(move.to, replica_summary.ts_uuid);
        tablet_uuids_dst.emplace_back(tablet_summary.id);
      }
    }
  }
  sort(tablet_uuids_src.begin(), tablet_uuids_src.end());
  sort(tablet_uuids_dst.begin(), tablet_uuids_dst.end());

  vector<string> tablet_uuids;
  set_difference(
      tablet_uuids_src.begin(), tablet_uuids_src.end(),
      tablet_uuids_dst.begin(), tablet_uuids_dst.end(),
      inserter(tablet_uuids, tablet_uuids.begin()));

  if (!tablet_uuids.empty()) {
    // If there are tablets with non-leader replicas at the source server,
    // those are the best candidates for movement.
    tablet_ids->swap(tablet_uuids);
    return Status::OK();
  }

  // If no tablets with non-leader replicas were found, resort to tablets with
  // leader replicas at the source server.
  DCHECK(tablet_uuids.empty());
  sort(tablet_uuids_src_leaders.begin(), tablet_uuids_src_leaders.end());
  set_difference(
      tablet_uuids_src_leaders.begin(), tablet_uuids_src_leaders.end(),
      tablet_uuids_dst.begin(), tablet_uuids_dst.end(),
      inserter(tablet_uuids, tablet_uuids.begin()));

  tablet_ids->swap(tablet_uuids);

  return Status::OK();
}

Status Rebalancer::ResetKsck() {
  shared_ptr<KsckCluster> cluster;
  RETURN_NOT_OK_PREPEND(
      RemoteKsckCluster::Build(config_.master_addresses, &cluster),
      "unable to build KsckCluster");
  ksck_.reset(new Ksck(cluster));
  ksck_->set_table_filters(config_.table_filters);
  return Status::OK();
}

void Rebalancer::FilterMoves(const MovesInProgress& scheduled_moves,
                             vector<ReplicaMove>* replica_moves) {
  unordered_set<string> tablet_uuids;
  vector<ReplicaMove> filtered_replica_moves;
  for (auto&& move_op : *replica_moves) {
    const auto& tablet_uuid = move_op.tablet_uuid;
    if (scheduled_moves.find(tablet_uuid) != scheduled_moves.end()) {
      // There is a move operation in progress for the tablet, don't schedule
      // another one.
      continue;
    }
    if (PREDICT_TRUE(tablet_uuids.emplace(tablet_uuid).second)) {
      filtered_replica_moves.push_back(std::move(move_op));
    } else {
      // Rationale behind the unique tablet constraint: the implementation of
      // the Run() method is designed to re-order operations suggested by the
      // high-level algorithm to use the op-count-per-tablet-server capacity
      // as much as possible. Right now, the RunStep() method outputs only one
      // move operation per tablet in every batch. The code below is to
      // enforce the contract between Run() and RunStep() methods.
      LOG(DFATAL) << "detected multiple replica move operations for the same "
                     "tablet " << tablet_uuid;
    }
  }
  replica_moves->swap(filtered_replica_moves);
}

Rebalancer::Runner::Runner(size_t max_moves_per_server,
                           const boost::optional<MonoTime>& deadline)
    : max_moves_per_server_(max_moves_per_server),
      deadline_(deadline),
      moves_count_(0) {
}

Status Rebalancer::Runner::Init(vector<string> master_addresses) {
  DCHECK_EQ(0, moves_count_);
  DCHECK(src_op_indices_.empty());
  DCHECK(dst_op_indices_.empty());
  DCHECK(op_count_per_ts_.empty());
  DCHECK(ts_per_op_count_.empty());
  DCHECK(scheduled_moves_.empty());
  DCHECK(master_addresses_.empty());
  DCHECK(client_.get() == nullptr);
  master_addresses_ = std::move(master_addresses);
  return KuduClientBuilder()
      .master_server_addrs(master_addresses_)
      .Build(&client_);
}

void Rebalancer::Runner::LoadMoves(vector<ReplicaMove> replica_moves) {
  // The moves to schedule (used by subsequent calls to ScheduleNextMove()).
  replica_moves_.swap(replica_moves);

  // Prepare helper containers.
  src_op_indices_.clear();
  dst_op_indices_.clear();
  op_count_per_ts_.clear();
  ts_per_op_count_.clear();

  // If there are any scheduled moves, it's necessary to count them in
  // to properly handle the 'maximum moves per server' constraint.
  unordered_map<string, int32_t> ts_pending_op_count;
  for (auto it = scheduled_moves_.begin(); it != scheduled_moves_.end(); ++it) {
    ++ts_pending_op_count[it->second.ts_uuid_from];
    ++ts_pending_op_count[it->second.ts_uuid_to];
  }

  // These two references is to make the compiler happy with the lambda below.
  auto& op_count_per_ts = op_count_per_ts_;
  auto& ts_per_op_count = ts_per_op_count_;
  const auto set_op_count = [&ts_pending_op_count,
      &op_count_per_ts, &ts_per_op_count](const string& ts_uuid) {
    auto it = ts_pending_op_count.find(ts_uuid);
    if (it == ts_pending_op_count.end()) {
      // No operations for tablet server ts_uuid yet.
      if (op_count_per_ts.emplace(ts_uuid, 0).second) {
        ts_per_op_count.emplace(0, ts_uuid);
      }
    } else {
      // There are pending operations for tablet server ts_uuid: set the number
      // operations at the tablet server ts_uuid as calculated above with
      // ts_pending_op_count.
      if (op_count_per_ts.emplace(ts_uuid, it->second).second) {
        ts_per_op_count.emplace(it->second, ts_uuid);
      }
      // Once set into op_count_per_ts and ts_per_op_count, this information
      // is no longer needed. In addition, these elements are removed to leave
      // only pending operations those do not intersect with the batch of newly
      // loaded operations.
      ts_pending_op_count.erase(it);
    }
  };

  // Process move operations from the batch of newly loaded ones.
  for (size_t i = 0; i < replica_moves_.size(); ++i) {
    const auto& elem = replica_moves_[i];
    src_op_indices_.emplace(elem.ts_uuid_from, set<size_t>()).first->
        second.emplace(i);
    set_op_count(elem.ts_uuid_from);

    dst_op_indices_.emplace(elem.ts_uuid_to, set<size_t>()).first->
        second.emplace(i);
    set_op_count(elem.ts_uuid_to);
  }

  // Process pending/scheduled move operations which do not intersect
  // with the batch of newly loaded ones.
  for (const auto& elem : ts_pending_op_count) {
    auto op_inserted = op_count_per_ts.emplace(elem.first, elem.second).second;
    DCHECK(op_inserted);
    ts_per_op_count.emplace(elem.second, elem.first);
  }
}

// Return true if replica move operation has been scheduled successfully.
bool Rebalancer::Runner::ScheduleNextMove(bool* has_errors, bool* timed_out) {
  DCHECK(has_errors);
  DCHECK(timed_out);
  *has_errors = false;
  *timed_out = false;

  if (deadline_ && MonoTime::Now() >= *deadline_) {
    *timed_out = true;
    return false;
  }

  // Only one move operation per step: it's necessary to update information
  // in the ts_per_op_count_ right after scheduling a single operation
  // to avoid oversubscribing of the tablet servers.
  size_t op_idx;
  if (!FindNextMove(&op_idx)) {
    // Nothing to schedule: unfruitful outcome. Need to wait until
    // there is a slot at tablet server is available.
    return false;
  }

  // Try to schedule next move operation.
  DCHECK_LT(op_idx, replica_moves_.size());
  const auto& info = replica_moves_[op_idx];
  const auto& tablet_id = info.tablet_uuid;
  const auto& src_ts_uuid = info.ts_uuid_from;
  const auto& dst_ts_uuid = info.ts_uuid_to;

  Status s = ScheduleReplicaMove(master_addresses_, client_,
                                 tablet_id, src_ts_uuid, dst_ts_uuid);
  if (s.ok()) {
    UpdateOnMoveScheduled(op_idx, info.tablet_uuid,
                          info.ts_uuid_from, info.ts_uuid_to, true);
    LOG(INFO) << Substitute("tablet $0: $1 -> $2 move scheduled",
                            tablet_id, src_ts_uuid, dst_ts_uuid);
    // Successfully scheduled move operation.
    return true;
  }

  DCHECK(!s.ok());
  // The source replica is not found in the tablet's consensus config
  // or the tablet does not exit anymore. The replica might already
  // moved because of some other concurrent activity, e.g.
  // re-replication, another rebalancing session in progress, etc.
  LOG(INFO) << Substitute("tablet $0: $1 -> $2 move ignored: $3",
                          tablet_id, src_ts_uuid, dst_ts_uuid, s.ToString());
  UpdateOnMoveScheduled(op_idx, info.tablet_uuid,
                        info.ts_uuid_from, info.ts_uuid_to, false);
  // Failed to schedule move operation due to an error.
  *has_errors = true;
  return false;
}

bool Rebalancer::Runner::UpdateMovesInProgressStatus(
    bool* has_errors, bool* timed_out) {
  DCHECK(has_errors);
  DCHECK(timed_out);
  *has_errors = false;
  *timed_out = false;

  // Update the statuses of the in-progress move operations.
  auto has_updates = false;
  auto error_count = 0;
  for (auto it = scheduled_moves_.begin(); it != scheduled_moves_.end(); ) {
    if (deadline_ && MonoTime::Now() >= *deadline_) {
      *timed_out = true;
      break;
    }
    const auto& tablet_id = it->first;
    DCHECK_EQ(tablet_id, it->second.tablet_uuid);
    const auto& src_ts_uuid = it->second.ts_uuid_from;
    const auto& dst_ts_uuid = it->second.ts_uuid_to;
    auto is_complete = false;
    Status move_status;
    const Status s = CheckCompleteMove(master_addresses_, client_,
                                       tablet_id, src_ts_uuid, dst_ts_uuid,
                                       &is_complete, &move_status);
    has_updates |= s.ok();
    if (!s.ok()) {
      // There was an error while fetching the status of this move operation.
      // Since the actual status of the move is not known, don't update the
      // stats on pending operations per server. The higher-level should handle
      // this situation after returning from this method, re-synchronizing
      // the state of the cluster.
      ++error_count;
      LOG(INFO) << Substitute("tablet $0: $1 -> $2 move is abandoned: $3",
                              tablet_id, src_ts_uuid, dst_ts_uuid, s.ToString());
      // Erase the element and advance the iterator.
      it = scheduled_moves_.erase(it);
      continue;
    } else if (is_complete) {
      // The move has completed (success or failure): update the stats on the
      // pending operations per server.
      ++moves_count_;
      UpdateOnMoveCompleted(it->second.ts_uuid_from);
      UpdateOnMoveCompleted(it->second.ts_uuid_to);
      LOG(INFO) << Substitute("tablet $0: $1 -> $2 move completed: $3",
                              tablet_id, src_ts_uuid, dst_ts_uuid,
                              s.ok() ? move_status.ToString() : s.ToString());
      // Erase the element and advance the iterator.
      it = scheduled_moves_.erase(it);
      continue;
    }
    // There was an update on the status of the move operation and it hasn't
    // completed yet. Let's poll for the status of the rest.
    ++it;
  }
  *has_errors = (error_count != 0);
  return has_updates;
}

bool Rebalancer::Runner::FindNextMove(size_t* op_idx) {
  vector<size_t> op_indices;
  for (auto it = ts_per_op_count_.begin(); op_indices.empty() &&
       it != ts_per_op_count_.end() && it->first < max_moves_per_server_; ++it) {
    const auto& uuid_0 = it->second;

    auto it_1 = it;
    ++it_1;
    for (; op_indices.empty() && it_1 != ts_per_op_count_.end() &&
         it_1->first < max_moves_per_server_; ++it_1) {
      const auto& uuid_1 = it_1->second;

      // Check for available operations where uuid_0, uuid_1 would be
      // source or destination servers correspondingly.
      {
        const auto it_src = src_op_indices_.find(uuid_0);
        const auto it_dst = dst_op_indices_.find(uuid_1);
        if (it_src != src_op_indices_.end() &&
            it_dst != dst_op_indices_.end()) {
          set_intersection(it_src->second.begin(), it_src->second.end(),
                           it_dst->second.begin(), it_dst->second.end(),
                           back_inserter(op_indices));
        }
      }
      // It's enough to find just one move.
      if (!op_indices.empty()) {
        break;
      }
      {
        const auto it_src = src_op_indices_.find(uuid_1);
        const auto it_dst = dst_op_indices_.find(uuid_0);
        if (it_src != src_op_indices_.end() &&
            it_dst != dst_op_indices_.end()) {
          set_intersection(it_src->second.begin(), it_src->second.end(),
                           it_dst->second.begin(), it_dst->second.end(),
                           back_inserter(op_indices));
        }
      }
    }
  }
  if (!op_indices.empty() && op_idx) {
    *op_idx = op_indices.front();
  }
  return !op_indices.empty();
}

void Rebalancer::Runner::UpdateOnMoveScheduled(
    size_t idx,
    const string& tablet_uuid,
    const string& src_ts_uuid,
    const string& dst_ts_uuid,
    bool is_success) {
  if (is_success) {
    Rebalancer::ReplicaMove move_info = { tablet_uuid, src_ts_uuid, dst_ts_uuid };
    auto ins = scheduled_moves_.emplace(tablet_uuid, std::move(move_info));
    // Only one replica of a tablet can be moved at a time.
    DCHECK(ins.second);
  }
  UpdateOnMoveScheduledImpl(idx, src_ts_uuid, is_success, &src_op_indices_);
  UpdateOnMoveScheduledImpl(idx, dst_ts_uuid, is_success, &dst_op_indices_);
}

void Rebalancer::Runner::UpdateOnMoveScheduledImpl(
    size_t idx,
    const string& ts_uuid,
    bool is_success,
    std::unordered_map<std::string, std::set<size_t>>* op_indices) {
  DCHECK(op_indices);
  auto& indices = (*op_indices)[ts_uuid];
  auto erased = indices.erase(idx);
  DCHECK_EQ(1, erased);
  if (indices.empty()) {
    op_indices->erase(ts_uuid);
  }
  if (is_success) {
    const auto op_count = op_count_per_ts_[ts_uuid]++;
    const auto op_range = ts_per_op_count_.equal_range(op_count);
    bool ts_op_count_updated = false;
    for (auto it = op_range.first; it != op_range.second; ++it) {
      if (it->second == ts_uuid) {
        ts_per_op_count_.erase(it);
        ts_per_op_count_.emplace(op_count + 1, ts_uuid);
        ts_op_count_updated = true;
        break;
      }
    }
    DCHECK(ts_op_count_updated);
  }
}

void Rebalancer::Runner::UpdateOnMoveCompleted(const string& ts_uuid) {
  const auto op_count = op_count_per_ts_[ts_uuid]--;
  const auto op_range = ts_per_op_count_.equal_range(op_count);
  bool ts_per_op_count_updated = false;
  for (auto it = op_range.first; it != op_range.second; ++it) {
    if (it->second == ts_uuid) {
      ts_per_op_count_.erase(it);
      ts_per_op_count_.emplace(op_count - 1, ts_uuid);
      ts_per_op_count_updated = true;
      break;
    }
  }
  DCHECK(ts_per_op_count_updated);
}

} // namespace tools
} // namespace kudu
