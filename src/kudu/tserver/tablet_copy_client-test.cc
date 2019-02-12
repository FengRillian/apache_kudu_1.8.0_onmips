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
#include "kudu/tserver/tablet_copy-test-base.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <ostream>
#include <stdlib.h>
#include <string>
#include <thread>
#include <vector>

#include <gflags/gflags.h>
#include <gflags/gflags_declare.h>
#include <glog/logging.h>
#include <glog/stl_logging.h>
#include <gtest/gtest.h>

#include "kudu/common/wire_protocol.h"
#include "kudu/consensus/consensus_meta_manager.h"
#include "kudu/consensus/log.h"
#include "kudu/consensus/log_reader.h"
#include "kudu/consensus/log_util.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/consensus/quorum_util.h"
#include "kudu/consensus/raft_consensus.h"
#include "kudu/fs/block_id.h"
#include "kudu/fs/block_manager.h"
#include "kudu/fs/data_dirs.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/casts.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/strings/fastmem.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/rpc/messenger.h"
#include "kudu/tablet/metadata.pb.h"
#include "kudu/tablet/tablet_metadata.h"
#include "kudu/tablet/tablet_replica.h"
#include "kudu/tserver/tablet_copy.pb.h"
#include "kudu/tserver/tablet_copy_client.h"
#include "kudu/util/crc.h"
#include "kudu/util/env.h"
#include "kudu/util/env_util.h"
#include "kudu/util/faststring.h"
#include "kudu/util/metrics.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/path_util.h"
#include "kudu/util/slice.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"

using std::shared_ptr;
using std::string;
using std::thread;
using std::vector;

DECLARE_double(env_inject_eio);
DECLARE_string(block_manager);
DECLARE_string(env_inject_eio_globs);

METRIC_DECLARE_counter(block_manager_total_disk_sync);

namespace kudu {
namespace tserver {

using consensus::ConsensusMetadataManager;
using consensus::ConsensusStatePB;
using consensus::GetRaftConfigLeader;
using consensus::RaftPeerPB;
using fs::DataDirManager;
using std::tuple;
using std::unique_ptr;
using strings::Substitute;
using tablet::TabletMetadata;

class TabletCopyClientTest : public TabletCopyTest {
 public:
  virtual void SetUp() OVERRIDE {
    NO_FATALS(TabletCopyTest::SetUp());

    // To be a bit more flexible in testing, create a FS layout with multiple disks.
    const string kTestWalDir = GetTestPath("client_tablet_wal");
    const string kTestDataDirPrefix = GetTestPath("client_tablet_data");
    FsManagerOpts opts;
    opts.wal_root = kTestWalDir;
    for (int dir = 0; dir < kNumDataDirs; dir++) {
      opts.data_roots.emplace_back(Substitute("$0-$1", kTestDataDirPrefix, dir));
    }

    metric_entity_ = METRIC_ENTITY_server.Instantiate(&metric_registry_, "test");
    opts.metric_entity = metric_entity_;
    fs_manager_.reset(new FsManager(Env::Default(), opts));
    ASSERT_OK(fs_manager_->CreateInitialFileSystemLayout());
    ASSERT_OK(fs_manager_->Open());
    ASSERT_OK(ResetTabletCopyClient());
  }

  // Sets up a new tablet copy client.
  Status ResetTabletCopyClient() {
    scoped_refptr<ConsensusMetadataManager> cmeta_manager(
        new ConsensusMetadataManager(fs_manager_.get()));

    tablet_replica_->WaitUntilConsensusRunning(MonoDelta::FromSeconds(10.0));
    rpc::MessengerBuilder(CURRENT_TEST_NAME()).Build(&messenger_);
    client_.reset(new TabletCopyClient(GetTabletId(),
                                       fs_manager_.get(),
                                       cmeta_manager,
                                       messenger_,
                                       nullptr /* no metrics */));
    RaftPeerPB* cstate_leader;
    ConsensusStatePB cstate;
    RETURN_NOT_OK(tablet_replica_->consensus()->ConsensusState(&cstate));
    RETURN_NOT_OK(GetRaftConfigLeader(&cstate, &cstate_leader));
    leader_ = *cstate_leader;
    return Status::OK();
  }

  // Starts the tablet copy.
  Status StartCopy() {
    HostPort host_port;
    RETURN_NOT_OK(HostPortFromPB(leader_.last_known_addr(), &host_port));
    return client_->Start(host_port, &meta_);
  }

 protected:
  Status CompareFileContents(const string& path1, const string& path2);

  MetricRegistry metric_registry_;
  scoped_refptr<MetricEntity> metric_entity_;
  unique_ptr<FsManager> fs_manager_;
  shared_ptr<rpc::Messenger> messenger_;
  unique_ptr<TabletCopyClient> client_;
  scoped_refptr<TabletMetadata> meta_;
  RaftPeerPB leader_;
};

Status TabletCopyClientTest::CompareFileContents(const string& path1, const string& path2) {
  shared_ptr<RandomAccessFile> file1, file2;
  RETURN_NOT_OK(env_util::OpenFileForRandom(fs_manager_->env(), path1, &file1));
  RETURN_NOT_OK(env_util::OpenFileForRandom(fs_manager_->env(), path2, &file2));

  uint64_t size1, size2;
  RETURN_NOT_OK(file1->Size(&size1));
  RETURN_NOT_OK(file2->Size(&size2));
  if (size1 != size2) {
    return Status::Corruption("Sizes of files don't match",
                              Substitute("$0 vs $1 bytes", size1, size2));
  }

  faststring scratch1, scratch2;
  scratch1.resize(size1);
  scratch2.resize(size2);
  Slice slice1(scratch1.data(), size1);
  Slice slice2(scratch2.data(), size2);
  RETURN_NOT_OK(file1->Read(0, slice1));
  RETURN_NOT_OK(file2->Read(0, slice2));
  int result = strings::fastmemcmp_inlined(slice1.data(), slice2.data(), size1);
  if (result != 0) {
    return Status::Corruption("Files do not match");
  }
  return Status::OK();
}

// Test a tablet copy going through the various states in the copy state
// machine.
TEST_F(TabletCopyClientTest, TestLifeCycle) {
  // Target fault injection for the tablet metadata directories, but do not
  // start injecting failures just yet.
  const vector<string> meta_dirs = {
      JoinPathSegments(client_->fs_manager_->GetConsensusMetadataDir(), "**"),
      JoinPathSegments(client_->fs_manager_->GetTabletMetadataDir(), "**") };
  FLAGS_env_inject_eio_globs = JoinStrings(meta_dirs, ",");

  ASSERT_EQ(TabletCopyClient::State::kInitialized, client_->state_);
  Status s;
  // If we're creating a brand new tablet, failing to start the copy will yield
  // no changes in-memory. It should be as if the copy hadn't started.
  {
    google::FlagSaver fs;
    FLAGS_env_inject_eio = 1.0;
    s = StartCopy();
    ASSERT_TRUE(s.IsIOError()) << s.ToString();
    ASSERT_STR_CONTAINS(s.ToString(), "Failed to write tablet metadata");
    ASSERT_EQ(TabletCopyClient::State::kInitialized, client_->state_);
    ASSERT_FALSE(meta_);
  }

  // Now let's try replacing a tablet. Set a metadata that we can replace.
  ASSERT_OK(ResetTabletCopyClient());
  ASSERT_OK(StartCopy());
  ASSERT_EQ(TabletCopyClient::State::kStarted, client_->state_);
  ASSERT_OK(client_->Finish());
  ASSERT_EQ(TabletCopyClient::State::kFinished, client_->state_);

  // Since we're going to replace the tablet, we need to tombstone the existing
  // metadata first.
  meta_->set_tablet_data_state(tablet::TABLET_DATA_TOMBSTONED);
  ASSERT_OK(ResetTabletCopyClient());
  ASSERT_OK(client_->SetTabletToReplace(meta_, 0));

  // If we're replacing a tablet, failing to start will yield changes
  // in-memory, and it is thus necessary to recognize the copy is in the
  // process of starting.
  {
    google::FlagSaver fs;
    FLAGS_env_inject_eio = 1.0;
    s = StartCopy();
    ASSERT_TRUE(s.IsIOError()) << s.ToString();
    ASSERT_STR_CONTAINS(s.ToString(), "Could not replace superblock");
    ASSERT_EQ(TabletCopyClient::State::kStarting, client_->state_);
  }

  // Make sure we are still in the appropriate state if we fail to finish.
  ASSERT_OK(ResetTabletCopyClient());
  client_->SetTabletToReplace(meta_, 0);
  ASSERT_OK(StartCopy());
  FLAGS_env_inject_eio = 1.0;
  s = client_->Finish();
  ASSERT_TRUE(s.IsIOError());
  ASSERT_EQ(TabletCopyClient::State::kStarted, client_->state_);

  // Closing out the copy should leave the copy client in its terminal state,
  // even upon failure.
  s = client_->Abort();
  ASSERT_TRUE(s.IsIOError());
  ASSERT_EQ(TabletCopyClient::State::kFinished, client_->state_);
  ASSERT_EQ(tablet::TABLET_DATA_TOMBSTONED, meta_->tablet_data_state());
}

// Implementation test that no blocks exist in the new superblock before fetching.
TEST_F(TabletCopyClientTest, TestNoBlocksAtStart) {
  ASSERT_OK(StartCopy());
  ASSERT_GT(ListBlocks(*client_->remote_superblock_).size(), 0);
  ASSERT_EQ(0, ListBlocks(*client_->superblock_).size());
}

// Basic begin / end tablet copy session.
TEST_F(TabletCopyClientTest, TestBeginEndSession) {
  ASSERT_OK(StartCopy());
  ASSERT_OK(client_->FetchAll(nullptr /* no listener */));
  ASSERT_OK(client_->Finish());
}

// Basic data block download unit test.
TEST_F(TabletCopyClientTest, TestDownloadBlock) {
  ASSERT_OK(StartCopy());
  BlockId block_id = FirstColumnBlockId(*client_->remote_superblock_);
  Slice slice;
  faststring scratch;

  // Ensure the block wasn't there before (it shouldn't be, we use our own FsManager dir).
  Status s;
  s = ReadLocalBlockFile(fs_manager_.get(), block_id, &scratch, &slice);
  ASSERT_TRUE(s.IsNotFound()) << "Expected block not found: " << s.ToString();

  // Check that the client downloaded the block and verification passed.
  BlockId new_block_id;
  ASSERT_OK(client_->DownloadBlock(block_id, &new_block_id));
  ASSERT_OK(client_->transaction_->CommitCreatedBlocks());

  // Ensure it placed the block where we expected it to.
  ASSERT_OK(ReadLocalBlockFile(fs_manager_.get(), new_block_id, &scratch, &slice));
}

// Basic WAL segment download unit test.
TEST_F(TabletCopyClientTest, TestDownloadWalSegment) {
  ASSERT_OK(StartCopy());
  ASSERT_OK(env_util::CreateDirIfMissing(
      env_, fs_manager_->GetTabletWalDir(GetTabletId())));

  uint64_t seqno = client_->wal_seqnos_[0];
  string path = fs_manager_->GetWalSegmentFileName(GetTabletId(), seqno);

  ASSERT_FALSE(fs_manager_->Exists(path));
  ASSERT_OK(client_->DownloadWAL(seqno));
  ASSERT_TRUE(fs_manager_->Exists(path));

  log::SegmentSequence local_segments;
  ASSERT_OK(tablet_replica_->log()->reader()->GetSegmentsSnapshot(&local_segments));
  const scoped_refptr<log::ReadableLogSegment>& segment = local_segments[0];
  string server_path = segment->path();

  // Compare the downloaded file with the source file.
  ASSERT_OK(CompareFileContents(path, server_path));
}

// Ensure that we detect data corruption at the per-transfer level.
TEST_F(TabletCopyClientTest, TestVerifyData) {
  ASSERT_OK(StartCopy());
  string good = "This is a known good string";
  string bad = "This is a known bad! string";
  const int kGoodOffset = 0;
  const int kBadOffset = 1;
  const int64_t kDataTotalLen = std::numeric_limits<int64_t>::max(); // Ignored.

  // Create a known-good PB.
  DataChunkPB valid_chunk;
  valid_chunk.set_offset(0);
  valid_chunk.set_data(good);
  valid_chunk.set_crc32(crc::Crc32c(good.data(), good.length()));
  valid_chunk.set_total_data_length(kDataTotalLen);

  // Make sure we work on the happy case.
  ASSERT_OK(client_->VerifyData(kGoodOffset, valid_chunk));

  // Test unexpected offset.
  DataChunkPB bad_offset = valid_chunk;
  bad_offset.set_offset(kBadOffset);
  Status s;
  s = client_->VerifyData(kGoodOffset, bad_offset);
  ASSERT_TRUE(s.IsInvalidArgument()) << "Bad offset expected: " << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "Offset did not match");
  LOG(INFO) << "Expected error returned: " << s.ToString();

  // Test bad checksum.
  DataChunkPB bad_checksum = valid_chunk;
  bad_checksum.set_data(bad);
  s = client_->VerifyData(kGoodOffset, bad_checksum);
  ASSERT_TRUE(s.IsCorruption()) << "Invalid checksum expected: " << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "CRC32 does not match");
  LOG(INFO) << "Expected error returned: " << s.ToString();
}

TEST_F(TabletCopyClientTest, TestDownloadAllBlocks) {
  ASSERT_OK(StartCopy());
  // Download and commit all the blocks.
  ASSERT_OK(client_->DownloadBlocks());
  ASSERT_OK(client_->transaction_->CommitCreatedBlocks());

  // Verify the disk synchronization count.
  // TODO(awong): These values have been determined to be safe empirically.
  // If kNumDataDirs changes, these values may also change. The point of this
  // test is to exemplify the difference in syncs between the log and file
  // block managers, but it would be nice to formulate a bound here.
  if (FLAGS_block_manager == "log") {
    ASSERT_GE(9, down_cast<Counter*>(
        metric_entity_->FindOrNull(METRIC_block_manager_total_disk_sync).get())->value());
  } else {
    ASSERT_GE(22, down_cast<Counter*>(
        metric_entity_->FindOrNull(METRIC_block_manager_total_disk_sync).get())->value());
  }

  // After downloading blocks, verify that the old and remote and local
  // superblock point to the same number of blocks.
  vector<BlockId> old_data_blocks = ListBlocks(*client_->remote_superblock_);
  vector<BlockId> new_data_blocks = ListBlocks(*client_->superblock_);
  ASSERT_EQ(old_data_blocks.size(), new_data_blocks.size());

  // Verify that the new blocks are all present.
  for (const BlockId& block_id : new_data_blocks) {
    unique_ptr<fs::ReadableBlock> block;
    ASSERT_OK(fs_manager_->OpenBlock(block_id, &block));
  }
}

// Test that failing a disk outside fo the tablet copy client will eventually
// stop the copy client and cause it to fail.
TEST_F(TabletCopyClientTest, TestFailedDiskStopsClient) {
  ASSERT_OK(StartCopy());
  DataDirManager* dd_manager = fs_manager_->dd_manager();

  // Repeatedly fetch files for the client.
  Status s;
  auto copy_thread = thread([&] {
    while (s.ok()) {
      s = client_->FetchAll(nullptr);
    }
  });

  // In a separate thread, mark one of the directories as failed (not the
  // metadata directory).
  while (true) {
    if (rand() % 10 == 0) {
      dd_manager->MarkDataDirFailed(1, "injected failure in non-client thread");
      LOG(INFO) << "INJECTING FAILURE";
      break;
    }
    SleepFor(MonoDelta::FromMilliseconds(50));
  }

  // The copy thread should stop and the copy client should return an error.
  copy_thread.join();
  ASSERT_TRUE(s.IsIOError());
}

enum DownloadBlocks {
  kDownloadBlocks,    // Fetch blocks from remote.
  kNoDownloadBlocks,  // Do not fetch blocks from remote.
};
enum DeleteTrigger {
  kAbortMethod, // Delete data via Abort().
  kDestructor,  // Delete data via destructor.
  kNoDelete     // Don't delete data.
};

struct AbortTestParams {
  DownloadBlocks download_blocks;
  DeleteTrigger delete_type;
};

class TabletCopyClientAbortTest : public TabletCopyClientTest,
                                  public ::testing::WithParamInterface<
                                      tuple<DownloadBlocks, DeleteTrigger>> {
 public:
  virtual void SetUp() override {
    TabletCopyClientTest::SetUp();
    ASSERT_OK(StartCopy());
  }
 protected:
  // Create the specified number of blocks with junk data for testing purposes.
  void CreateTestBlocks(int num_blocks);
};

INSTANTIATE_TEST_CASE_P(BlockDeleteTriggers,
                        TabletCopyClientAbortTest,
                        ::testing::Combine(
                            ::testing::Values(kDownloadBlocks, kNoDownloadBlocks),
                            ::testing::Values(kAbortMethod, kDestructor, kNoDelete)));

void TabletCopyClientAbortTest::CreateTestBlocks(int num_blocks) {
  for (int i = 0; i < num_blocks; i++) {
    unique_ptr<fs::WritableBlock> block;
    ASSERT_OK(fs_manager_->CreateNewBlock({}, &block));
    block->Append("Test");
    ASSERT_OK(block->Close());
  }
}

// Test that we can clean up our downloaded blocks either explicitly using
// Abort() or implicitly by destroying the TabletCopyClient instance before
// calling Finish(). Also ensure that no data loss occurs.
TEST_P(TabletCopyClientAbortTest, TestAbort) {
  tuple<DownloadBlocks, DeleteTrigger> param = GetParam();
  DownloadBlocks download_blocks = std::get<0>(param);
  DeleteTrigger trigger = std::get<1>(param);

  // Check that there are remote blocks.
  vector<BlockId> remote_block_ids = ListBlocks(*client_->remote_superblock_);
  ASSERT_FALSE(remote_block_ids.empty());
  int num_remote_blocks = client_->CountRemoteBlocks();
  ASSERT_GT(num_remote_blocks, 0);
  ASSERT_EQ(num_remote_blocks, remote_block_ids.size());

  // Create some local blocks so we can check that we didn't lose any existing
  // data on abort. TODO(mpercy): The data loss check here will likely never
  // trigger until we fix KUDU-1980 because there is a workaround / hack in the
  // LBM that randomizes the starting block id for each BlockManager instance.
  // Therefore the block ids will never overlap.
  const int kNumBlocksToCreate = 100;
  NO_FATALS(CreateTestBlocks(kNumBlocksToCreate));

  vector<BlockId> local_block_ids;
  ASSERT_OK(fs_manager_->block_manager()->GetAllBlockIds(&local_block_ids));
  ASSERT_EQ(kNumBlocksToCreate, local_block_ids.size());
  VLOG(1) << "Local blocks: " << local_block_ids;

  int num_blocks_downloaded = 0;
  if (download_blocks == kDownloadBlocks) {
    ASSERT_OK(client_->DownloadBlocks());
    ASSERT_OK(client_->transaction_->CommitCreatedBlocks());
    num_blocks_downloaded = num_remote_blocks;
  }

  vector<BlockId> new_local_block_ids;
  ASSERT_OK(fs_manager_->block_manager()->GetAllBlockIds(&new_local_block_ids));
  ASSERT_EQ(kNumBlocksToCreate + num_blocks_downloaded, new_local_block_ids.size());

  // Download a WAL segment.
  ASSERT_OK(env_util::CreateDirIfMissing(
      env_, fs_manager_->GetTabletWalDir(GetTabletId())));
  uint64_t seqno = client_->wal_seqnos_[0];
  ASSERT_OK(client_->DownloadWAL(seqno));
  string wal_path = fs_manager_->GetWalSegmentFileName(GetTabletId(), seqno);
  ASSERT_TRUE(fs_manager_->Exists(wal_path));

  scoped_refptr<TabletMetadata> meta = client_->meta_;

  switch (trigger) {
    case kAbortMethod:
      ASSERT_OK(client_->Abort());
      break;
    case kDestructor:
      client_.reset();
      break;
    case kNoDelete:
      // Call Finish() and then destroy the object.
      // It should not delete its downloaded blocks.
      ASSERT_OK(client_->Finish());
      client_.reset();
      break;
    default:
      FAIL();
  }

  if (trigger == kNoDelete) {
    vector<BlockId> new_local_block_ids;
    ASSERT_OK(fs_manager_->block_manager()->GetAllBlockIds(&new_local_block_ids));
    ASSERT_EQ(kNumBlocksToCreate + num_blocks_downloaded, new_local_block_ids.size());
  } else {
    ASSERT_EQ(tablet::TABLET_DATA_TOMBSTONED, meta->tablet_data_state());
    ASSERT_FALSE(fs_manager_->Exists(wal_path));
    vector<BlockId> latest_blocks;
    fs_manager_->block_manager()->GetAllBlockIds(&latest_blocks);
    ASSERT_EQ(local_block_ids.size(), latest_blocks.size());
  }
  for (const auto& block_id : local_block_ids) {
    ASSERT_TRUE(fs_manager_->BlockExists(block_id)) << "Missing block: " << block_id;
  }
}

} // namespace tserver
} // namespace kudu
