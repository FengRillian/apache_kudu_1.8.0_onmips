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
//
// This is an integration test similar to TestLoadAndVerify in HBase.
// It creates a table and writes linked lists into it, where each row
// points to the previously written row. For example, a sequence of inserts
// may be:
//
//  rand_key   | link_to   |  insert_ts
//   12345          0           1
//   823          12345         2
//   9999          823          3
// (each insert links to the key of the previous insert)
//
// During insertion, a configurable number of parallel chains may be inserted.
// To verify, the table is scanned, and we ensure that every key is linked to
// either zero or one times, and no link_to refers to a missing key.

#include <cstdint>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/bind.hpp>
#include <gflags/gflags.h>
#include <gflags/gflags_declare.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/client/shared_ptr.h"
#include "kudu/common/wire_protocol.pb.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/port.h"
#include "kudu/integration-tests/cluster_itest_util.h"
#include "kudu/integration-tests/linked_list-test-util.h"
#include "kudu/integration-tests/ts_itest-base.h"
#include "kudu/master/master.pb.h"
#include "kudu/mini-cluster/external_mini_cluster.h"
#include "kudu/mini-cluster/mini_cluster.h"
#include "kudu/tserver/tablet_server-test-base.h"
#include "kudu/util/monotime.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

DECLARE_int32(num_replicas);
DECLARE_int32(num_tablet_servers);
DECLARE_string(ts_flags);
DEFINE_int32(seconds_to_run, 5, "Number of seconds for which to run the test");

DEFINE_int32(num_chains, 50, "Number of parallel chains to generate");
DEFINE_int32(num_tablets, 3, "Number of tablets over which to split the data");
DEFINE_bool(enable_mutation, true, "Enable periodic mutation of inserted rows");
DEFINE_int32(num_snapshots, 3, "Number of snapshots to verify across replicas and reboots.");

DEFINE_bool(stress_flush_compact, false,
            "Flush and compact way more aggressively to try to find bugs");
DEFINE_bool(stress_wal_gc, false,
            "Set WAL segment size small so that logs will be GCed during the test");

using kudu::client::sp::shared_ptr;
using kudu::cluster::ClusterNodes;
using kudu::itest::TServerDetails;
using kudu::itest::WAIT_FOR_LEADER;
using kudu::itest::WaitForReplicasReportedToMaster;
using kudu::itest::WaitForServersToAgree;
using kudu::master::VOTER_REPLICA;
using std::string;
using std::vector;

namespace kudu {

namespace client {
class KuduClient;
} // namespace client

class LinkedListTest : public tserver::TabletServerIntegrationTestBase {
 public:
  LinkedListTest() {}

  void SetUp() OVERRIDE {
    TabletServerIntegrationTestBase::SetUp();

    LOG(INFO) << "Linked List Test Configuration:";
    LOG(INFO) << "--------------";
    LOG(INFO) << FLAGS_num_chains << " chains";
    LOG(INFO) << FLAGS_num_tablets << " tablets";
    LOG(INFO) << "Mutations " << (FLAGS_enable_mutation ? "on" : "off");
    LOG(INFO) << "--------------";
  }

  void BuildAndStart() {
    vector<string> common_flags;

    common_flags.emplace_back("--skip_remove_old_recovery_dir");

    // Set history retention to one day, so that we don't GC history in this test.
    // We rely on verifying "back in time" with snapshot scans.
    common_flags.emplace_back("--tablet_history_max_age_sec=86400");

    vector<string> ts_flags(common_flags);
    if (FLAGS_stress_flush_compact) {
      // Set the flush threshold low so that we have a mix of flushed and unflushed
      // operations in the WAL, when we bootstrap.
      ts_flags.emplace_back("--flush_threshold_mb=1");
      // Set the compaction budget to be low so that we get multiple passes of compaction
      // instead of selecting all of the rowsets in a single compaction of the whole
      // tablet.
      ts_flags.emplace_back("--tablet_compaction_budget_mb=4");
      // Set the major delta compaction ratio low enough that we trigger a lot of them.
      ts_flags.emplace_back("--tablet_delta_store_major_compact_min_ratio=0.001");
    }
    if (FLAGS_stress_wal_gc) {
      // Set the size of the WAL segments low so that some can be GC'd.
      ts_flags.emplace_back("--log_segment_size_mb=1");
    }

    NO_FATALS(CreateCluster("linked-list-cluster", ts_flags, common_flags));
    ResetClientAndTester();
    ASSERT_OK(tester_->CreateLinkedListTable());
    WaitForTSAndReplicas();
  }

  void ResetClientAndTester() {
    ASSERT_OK(cluster_->CreateClient(nullptr, &client_));
    tester_.reset(new LinkedListTester(client_, kTableId,
                                       FLAGS_num_chains,
                                       FLAGS_num_tablets,
                                       FLAGS_num_replicas,
                                       FLAGS_enable_mutation));
  }

  void RestartCluster() {
    CHECK(cluster_);
    cluster_->ShutdownNodes(ClusterNodes::TS_ONLY);
    cluster_->Restart();
    ResetClientAndTester();
  }

 protected:
  shared_ptr<client::KuduClient> client_;
  gscoped_ptr<LinkedListTester> tester_;
};

TEST_F(LinkedListTest, TestLoadAndVerify) {
  OverrideFlagForSlowTests("seconds_to_run", "30");
  OverrideFlagForSlowTests("stress_flush_compact", "true");
  OverrideFlagForSlowTests("stress_wal_gc", "true");
  NO_FATALS(BuildAndStart());

  string tablet_id = tablet_replicas_.begin()->first;

  // In TSAN builds, we hit the web UIs more often, so we have a better chance
  // of seeing a thread error. We don't do this in normal builds since we
  // also use this test as a benchmark and it soaks up a lot of CPU.
#ifdef THREAD_SANITIZER
  MonoDelta check_freq = MonoDelta::FromMilliseconds(10);
#else
  MonoDelta check_freq = MonoDelta::FromSeconds(1);
#endif

  PeriodicWebUIChecker checker(*cluster_.get(), tablet_id,
                               check_freq);

  bool can_kill_ts = FLAGS_num_tablet_servers > 1 && FLAGS_num_replicas > 2;

  int64_t written = 0;
  ASSERT_OK(tester_->LoadLinkedList(MonoDelta::FromSeconds(FLAGS_seconds_to_run),
                                           FLAGS_num_snapshots,
                                           &written));

  // TODO: currently we don't use hybridtime on the C++ client, so it's possible when we
  // scan after writing we may not see all of our writes (we may scan a replica). So,
  // we use WaitAndVerify here instead of a plain Verify.
  ASSERT_OK(tester_->WaitAndVerify(FLAGS_seconds_to_run, written));
  ASSERT_OK(CheckTabletServersAreAlive(tablet_servers_.size()));

  LOG(INFO) << "Successfully verified " << written << " rows before killing any servers.";

  if (can_kill_ts) {
    // Restart a tserver during a scan to test scanner fault tolerance.
    WaitForTSAndReplicas();
    LOG(INFO) << "Will restart the tablet server during verification scan.";
    ASSERT_OK(tester_->WaitAndVerify(FLAGS_seconds_to_run, written,
                                     boost::bind(
                                         &TabletServerIntegrationTestBase::RestartServerWithUUID,
                                         this, _1)));
    LOG(INFO) << "Done with tserver restart test.";
    ASSERT_OK(CheckTabletServersAreAlive(tablet_servers_.size()));

    // Kill a tserver during a scan to test scanner fault tolerance.
    // Note that the previously restarted node is likely still be bootstrapping, which makes this
    // even harder.
    LOG(INFO) << "Will kill the tablet server during verification scan.";
    ASSERT_OK(tester_->WaitAndVerify(FLAGS_seconds_to_run, written,
                                     boost::bind(
                                         &TabletServerIntegrationTestBase::ShutdownServerWithUUID,
                                         this, _1)));
    LOG(INFO) << "Done with tserver kill test.";
    ASSERT_OK(CheckTabletServersAreAlive(tablet_servers_.size()-1));
    ASSERT_NO_FATAL_FAILURE(RestartCluster());
    // Again wait for cluster to finish bootstrapping.
    WaitForTSAndReplicas();

    // Check in-memory state with a downed TS. Scans may try other replicas.
    string tablet = (*tablet_replicas_.begin()).first;
    TServerDetails* leader;
    EXPECT_OK(GetLeaderReplicaWithRetries(tablet, &leader));
    LOG(INFO) << "Killing TS: " << leader->instance_id.permanent_uuid() << ", leader of tablet: "
        << tablet << " and verifying that we can still read all results";
    ASSERT_OK(ShutdownServerWithUUID(leader->uuid()));
    ASSERT_OK(tester_->WaitAndVerify(FLAGS_seconds_to_run, written));
    ASSERT_OK(CheckTabletServersAreAlive(tablet_servers_.size() - 1));
  }

  // Kill and restart the cluster, verify data remains.
  ASSERT_NO_FATAL_FAILURE(RestartCluster());

  LOG(INFO) << "Verifying rows after restarting entire cluster.";

  // We need to loop here because the tablet may spend some time in BOOTSTRAPPING state
  // initially after a restart. TODO: Scanner should support its own retries in this circumstance.
  // Remove this loop once client is more fleshed out.
  ASSERT_OK(tester_->WaitAndVerify(FLAGS_seconds_to_run, written));

  // In slow tests mode, we'll wait for a little bit to allow time for the tablet to
  // compact. This is a regression test for bugs where compaction post-bootstrap
  // could cause data loss.
  if (AllowSlowTests()) {
    SleepFor(MonoDelta::FromSeconds(10));
    ASSERT_OK(tester_->WaitAndVerify(FLAGS_seconds_to_run, written));
  }
  ASSERT_OK(CheckTabletServersAreAlive(tablet_servers_.size()));

  // Check post-replication state with a downed TS.
  if (can_kill_ts) {
    string tablet = (*tablet_replicas_.begin()).first;
    TServerDetails* leader;
    EXPECT_OK(GetLeaderReplicaWithRetries(tablet, &leader));
    LOG(INFO) << "Killing TS: " << leader->instance_id.permanent_uuid() << ", leader of tablet: "
        << tablet << " and verifying that we can still read all results";
    ASSERT_OK(ShutdownServerWithUUID(leader->uuid()));
    ASSERT_OK(tester_->WaitAndVerify(FLAGS_seconds_to_run, written));
    ASSERT_OK(CheckTabletServersAreAlive(tablet_servers_.size() - 1));
  }

  ASSERT_NO_FATAL_FAILURE(RestartCluster());

  // Sleep a little bit, so that the tablet is probably in bootstrapping state.
  SleepFor(MonoDelta::FromMilliseconds(100));

  // Restart while bootstrapping
  ASSERT_NO_FATAL_FAILURE(RestartCluster());

  ASSERT_OK(tester_->WaitAndVerify(FLAGS_seconds_to_run, written));
  ASSERT_OK(CheckTabletServersAreAlive(tablet_servers_.size()));

  // Dump the performance info at the very end, so it's easy to read. On a failed
  // test, we don't care about this stuff anyway.
  tester_->DumpInsertHistogram(true);
}

// This test loads the linked list while one of the servers is down.
// Once the loading is complete, the server is started back up and
// we wait for it to catch up. Then we shut down the other two servers
// and verify that the data is correct on the server which caught up.
TEST_F(LinkedListTest, TestLoadWhileOneServerDownAndVerify) {
  OverrideFlagForSlowTests("seconds_to_run", "30");

  if (!FLAGS_ts_flags.empty()) {
    FLAGS_ts_flags += " ";
  }

  FLAGS_ts_flags += "--log_cache_size_limit_mb=2";
  FLAGS_ts_flags += " --global_log_cache_size_limit_mb=4";

  FLAGS_num_tablet_servers = 3;
  FLAGS_num_tablets = 1;

  const auto run_time = MonoDelta::FromSeconds(FLAGS_seconds_to_run);
  const auto wait_time = run_time;

  NO_FATALS(BuildAndStart());

  // Load the data with one of the three servers down.
  cluster_->tablet_server(0)->Shutdown();

  int64_t written;
  ASSERT_OK(tester_->LoadLinkedList(run_time, FLAGS_num_snapshots, &written));

  // Start back up the server that missed all of the data being loaded. It should be
  // able to stream the data back from the other server which is still up.
  ASSERT_OK(cluster_->tablet_server(0)->Restart());

  string tablet_id = tablet_replicas_.begin()->first;

  // When running for longer times (like --seconds_to_run=800), the replica
  // at the shutdown tservers falls behind the WAL segment GC threshold. In case
  // of the 3-4-3 replica management scheme, that leads to evicting the former
  // replica and replacing it with a non-voter one. The WaitForServersToAgree()
  // below doesn't take into account that a non-voter replica, even caught up
  // with the leader, first needs to be promoted to voter before commencing the
  // verification phase. So, before checking for the minimum required operaiton
  // index, let's first make sure that the replica at the restarted tserver
  // is a voter.
  bool has_leader;
  master::TabletLocationsPB tablet_locations;
  ASSERT_OK(WaitForReplicasReportedToMaster(
      cluster_->master_proxy(), FLAGS_num_tablet_servers, tablet_id, wait_time,
      WAIT_FOR_LEADER, VOTER_REPLICA, &has_leader, &tablet_locations));

  // All right, having the necessary number of voter replicas, make sure all
  // replicas are up-to-date in terms of OpId index.
  ASSERT_OK(WaitForServersToAgree(wait_time, tablet_servers_, tablet_id,
                                  written / FLAGS_num_chains));

  cluster_->tablet_server(1)->Shutdown();
  cluster_->tablet_server(2)->Shutdown();

  ASSERT_OK(tester_->WaitAndVerify(FLAGS_seconds_to_run,
                                   written,
                                   LinkedListTester::FINISH_WITH_SCAN_LATEST));
}

} // namespace kudu
