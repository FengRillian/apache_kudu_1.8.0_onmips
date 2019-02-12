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

#include "kudu/tserver/ts_tablet_manager.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/common/common.pb.h"
#include "kudu/common/partition.h"
#include "kudu/common/schema.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/consensus/opid_util.h"
#include "kudu/consensus/raft_consensus.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/master/master.pb.h"
#include "kudu/tablet/tablet.h"
#include "kudu/tablet/tablet-harness.h"
#include "kudu/tablet/tablet_replica.h"
#include "kudu/tserver/heartbeater.h"
#include "kudu/tserver/mini_tablet_server.h"
#include "kudu/tserver/tablet_server.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

#define ASSERT_REPORT_HAS_UPDATED_TABLET(report, tablet_id) \
  ASSERT_NO_FATAL_FAILURE(AssertReportHasUpdatedTablet(report, tablet_id))

#define ASSERT_MONOTONIC_REPORT_SEQNO(report_seqno, tablet_report) \
  ASSERT_NO_FATAL_FAILURE(AssertMonotonicReportSeqno(report_seqno, tablet_report))

using std::string;
using std::vector;

namespace kudu {

class FsManager;

namespace tserver {

using consensus::kInvalidOpIdIndex;
using consensus::RaftConfigPB;
using master::ReportedTabletPB;
using master::TabletReportPB;
using pb_util::SecureShortDebugString;
using tablet::TabletReplica;

static const char* const kTabletId = "ffffffffffffffffffffffffffffffff";


class TsTabletManagerTest : public KuduTest {
 public:
  TsTabletManagerTest()
    : schema_({ ColumnSchema("key", UINT32) }, 1) {
  }

  virtual void SetUp() OVERRIDE {
    KuduTest::SetUp();
    mini_server_.reset(new MiniTabletServer(GetTestPath("TsTabletManagerTest-fsroot"),
                                            HostPort("127.0.0.1", 0)));
    ASSERT_OK(mini_server_->Start());
    mini_server_->FailHeartbeats();

    config_ = mini_server_->CreateLocalConfig();

    tablet_manager_ = mini_server_->server()->tablet_manager();
    fs_manager_ = mini_server_->server()->fs_manager();
    heartbeater_ = mini_server_->server()->heartbeater();
  }

  virtual void TearDown() OVERRIDE {
    KuduTest::TearDown();
  }

  Status CreateNewTablet(const std::string& tablet_id,
                         const Schema& schema,
                         scoped_refptr<tablet::TabletReplica>* out_tablet_replica) {
    Schema full_schema = SchemaBuilder(schema).Build();
    std::pair<PartitionSchema, Partition> partition = tablet::CreateDefaultPartition(full_schema);

    scoped_refptr<tablet::TabletReplica> tablet_replica;
    RETURN_NOT_OK(tablet_manager_->CreateNewTablet(tablet_id, tablet_id, partition.second,
                                                   tablet_id,
                                                   full_schema, partition.first,
                                                   config_,
                                                   &tablet_replica));
    if (out_tablet_replica) {
      (*out_tablet_replica) = tablet_replica;
    }

    RETURN_NOT_OK(tablet_replica->WaitUntilConsensusRunning(MonoDelta::FromMilliseconds(2000)));
    return tablet_replica->consensus()->WaitUntilLeaderForTests(MonoDelta::FromSeconds(10));
  }

  void GenerateFullTabletReport(TabletReportPB* report) {
    vector<TabletReportPB> reports =
        heartbeater_->GenerateFullTabletReportsForTests();
    ASSERT_EQ(1, reports.size());
    report->CopyFrom(reports[0]);
  }

  void GenerateIncrementalTabletReport(TabletReportPB* report) {
    vector<TabletReportPB> reports =
        heartbeater_->GenerateIncrementalTabletReportsForTests();
    ASSERT_EQ(1, reports.size());
    report->CopyFrom(reports[0]);
  }

  void MarkTabletReportAcknowledged(const TabletReportPB& report) {
    heartbeater_->MarkTabletReportsAcknowledgedForTests({ report });
  }

 protected:
  gscoped_ptr<MiniTabletServer> mini_server_;
  FsManager* fs_manager_;
  TSTabletManager* tablet_manager_;
  Heartbeater* heartbeater_;

  Schema schema_;
  RaftConfigPB config_;
};

TEST_F(TsTabletManagerTest, TestCreateTablet) {
  // Create a new tablet.
  scoped_refptr<TabletReplica> replica;
  ASSERT_OK(CreateNewTablet(kTabletId, schema_, &replica));
  ASSERT_EQ(kTabletId, replica->tablet()->tablet_id());
  replica.reset();

  // Re-load the tablet manager from the filesystem.
  LOG(INFO) << "Shutting down tablet manager";
  mini_server_->Shutdown();
  LOG(INFO) << "Restarting tablet manager";
  mini_server_.reset(new MiniTabletServer(GetTestPath("TsTabletManagerTest-fsroot"),
                                          HostPort("127.0.0.1", 0)));
  ASSERT_OK(mini_server_->Start());
  ASSERT_OK(mini_server_->WaitStarted());
  tablet_manager_ = mini_server_->server()->tablet_manager();

  // Ensure that the tablet got re-loaded and re-opened off disk.
  ASSERT_TRUE(tablet_manager_->LookupTablet(kTabletId, &replica));
  ASSERT_EQ(kTabletId, replica->tablet()->tablet_id());
}

static void AssertMonotonicReportSeqno(int64_t* report_seqno,
                                       const TabletReportPB &report) {
  ASSERT_LT(*report_seqno, report.sequence_number());
  *report_seqno = report.sequence_number();
}

static void AssertReportHasUpdatedTablet(const TabletReportPB& report,
                                         const string& tablet_id) {
  ASSERT_GE(report.updated_tablets_size(), 0);
  bool found_tablet = false;
  for (const ReportedTabletPB& reported_tablet : report.updated_tablets()) {
    if (reported_tablet.tablet_id() == tablet_id) {
      found_tablet = true;
      ASSERT_TRUE(reported_tablet.has_consensus_state());
      ASSERT_TRUE(reported_tablet.consensus_state().has_current_term())
          << SecureShortDebugString(reported_tablet);
      ASSERT_FALSE(reported_tablet.consensus_state().leader_uuid().empty())
          << SecureShortDebugString(reported_tablet);
      ASSERT_TRUE(reported_tablet.consensus_state().has_committed_config());
      const RaftConfigPB& committed_config = reported_tablet.consensus_state().committed_config();
      ASSERT_EQ(kInvalidOpIdIndex, committed_config.opid_index());
      ASSERT_EQ(1, committed_config.peers_size());
      ASSERT_TRUE(committed_config.peers(0).has_permanent_uuid())
          << SecureShortDebugString(reported_tablet);
      ASSERT_EQ(committed_config.peers(0).permanent_uuid(),
                reported_tablet.consensus_state().leader_uuid())
          << SecureShortDebugString(reported_tablet);
    }
  }
  ASSERT_TRUE(found_tablet);
}

TEST_F(TsTabletManagerTest, TestTabletReports) {
  TabletReportPB report;
  int64_t seqno = -1;

  // Generate a tablet report before any tablets are loaded. Should be empty.
  GenerateFullTabletReport(&report);
  ASSERT_FALSE(report.is_incremental());
  ASSERT_EQ(0, report.updated_tablets().size());
  ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
  MarkTabletReportAcknowledged(report);

  // Another report should now be incremental, but with no changes.
  GenerateIncrementalTabletReport(&report);
  ASSERT_TRUE(report.is_incremental());
  ASSERT_EQ(0, report.updated_tablets().size());
  ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
  MarkTabletReportAcknowledged(report);

  // Create a tablet and do another incremental report - should include the tablet.
  ASSERT_OK(CreateNewTablet("tablet-1", schema_, nullptr));
  int updated_tablets = 0;
  while (updated_tablets != 1) {
    GenerateIncrementalTabletReport(&report);
    updated_tablets = report.updated_tablets().size();
    ASSERT_TRUE(report.is_incremental());
    ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
  }

  ASSERT_REPORT_HAS_UPDATED_TABLET(report, "tablet-1");

  // If we don't acknowledge the report, and ask for another incremental report,
  // it should include the tablet again.
  GenerateIncrementalTabletReport(&report);
  ASSERT_TRUE(report.is_incremental());
  ASSERT_EQ(1, report.updated_tablets().size());
  ASSERT_REPORT_HAS_UPDATED_TABLET(report, "tablet-1");
  ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);

  // Now acknowledge the last report, and further incrementals should be empty.
  MarkTabletReportAcknowledged(report);
  GenerateIncrementalTabletReport(&report);
  ASSERT_TRUE(report.is_incremental());
  ASSERT_EQ(0, report.updated_tablets().size());
  ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
  MarkTabletReportAcknowledged(report);

  // Create a second tablet, and ensure the incremental report shows it.
  ASSERT_OK(CreateNewTablet("tablet-2", schema_, nullptr));

  // Wait up to 10 seconds to get a tablet report from tablet-2.
  // TabletReplica does not mark tablets dirty until after it commits the
  // initial configuration change, so there is also a window for tablet-1 to
  // have been marked dirty since the last report.
  MonoDelta timeout(MonoDelta::FromSeconds(10));
  MonoTime start(MonoTime::Now());
  report.Clear();
  while (true) {
    bool found_tablet_2 = false;
    GenerateIncrementalTabletReport(&report);
    ASSERT_TRUE(report.is_incremental()) << SecureShortDebugString(report);
    ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report) << SecureShortDebugString(report);
    for (const ReportedTabletPB& reported_tablet : report.updated_tablets()) {
      if (reported_tablet.tablet_id() == "tablet-2") {
        found_tablet_2  = true;
        break;
      }
    }
    if (found_tablet_2) break;
    MonoDelta elapsed(MonoTime::Now() - start);
    ASSERT_TRUE(elapsed < timeout)
        << "Waited too long for tablet-2 to be marked dirty: "
        << elapsed.ToString() << ". "
        << "Latest report: " << SecureShortDebugString(report);
    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  MarkTabletReportAcknowledged(report);

  // Asking for a full tablet report should re-report both tablets
  GenerateFullTabletReport(&report);
  ASSERT_FALSE(report.is_incremental());
  ASSERT_EQ(2, report.updated_tablets().size());
  ASSERT_REPORT_HAS_UPDATED_TABLET(report, "tablet-1");
  ASSERT_REPORT_HAS_UPDATED_TABLET(report, "tablet-2");
  ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
}

} // namespace tserver
} // namespace kudu
