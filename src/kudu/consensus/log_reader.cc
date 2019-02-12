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

#include "kudu/consensus/log_reader.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <ostream>

#include <glog/logging.h>

#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/log.pb.h"
#include "kudu/consensus/log_index.h"
#include "kudu/consensus/opid.pb.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/stl_util.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/util/env.h"
#include "kudu/util/faststring.h"
#include "kudu/util/metrics.h"
#include "kudu/util/path_util.h"
#include "kudu/util/pb_util.h"

METRIC_DEFINE_counter(tablet, log_reader_bytes_read, "Bytes Read From Log",
                      kudu::MetricUnit::kBytes,
                      "Data read from the WAL since tablet start");

METRIC_DEFINE_counter(tablet, log_reader_entries_read, "Entries Read From Log",
                      kudu::MetricUnit::kEntries,
                      "Number of entries read from the WAL since tablet start");

METRIC_DEFINE_histogram(tablet, log_reader_read_batch_latency, "Log Read Latency",
                        kudu::MetricUnit::kBytes,
                        "Microseconds spent reading log entry batches",
                        60000000LU, 2);

using kudu::consensus::OpId;
using kudu::consensus::ReplicateMsg;
using kudu::pb_util::SecureDebugString;
using kudu::pb_util::SecureShortDebugString;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;
using strings::Substitute;

namespace kudu {
namespace log {

namespace {
struct LogSegmentSeqnoComparator {
  bool operator() (const scoped_refptr<ReadableLogSegment>& a,
                   const scoped_refptr<ReadableLogSegment>& b) {
    return a->header().sequence_number() < b->header().sequence_number();
  }
};
}

const int64_t LogReader::kNoSizeLimit = -1;

Status LogReader::Open(Env* env,
                       const string& tablet_wal_dir,
                       const scoped_refptr<LogIndex>& index,
                       const string& tablet_id,
                       const scoped_refptr<MetricEntity>& metric_entity,
                       shared_ptr<LogReader>* reader) {
  auto log_reader = LogReader::make_shared(env, index, tablet_id, metric_entity);

  RETURN_NOT_OK_PREPEND(log_reader->Init(tablet_wal_dir),
                        "Unable to initialize log reader")
  *reader = log_reader;
  return Status::OK();
}

Status LogReader::Open(FsManager* fs_manager,
                       const scoped_refptr<LogIndex>& index,
                       const std::string& tablet_id,
                       const scoped_refptr<MetricEntity>& metric_entity,
                       std::shared_ptr<LogReader>* reader) {
  return LogReader::Open(fs_manager->env(), fs_manager->GetTabletWalDir(tablet_id),
                         index, tablet_id, metric_entity, reader);
}

LogReader::LogReader(Env* env,
                     scoped_refptr<LogIndex> index,
                     string tablet_id,
                     const scoped_refptr<MetricEntity>& metric_entity)
    : env_(env),
      log_index_(std::move(index)),
      tablet_id_(std::move(tablet_id)),
      state_(kLogReaderInitialized) {
  if (metric_entity) {
    bytes_read_ = METRIC_log_reader_bytes_read.Instantiate(metric_entity);
    entries_read_ = METRIC_log_reader_entries_read.Instantiate(metric_entity);
    read_batch_latency_ = METRIC_log_reader_read_batch_latency.Instantiate(metric_entity);
  }
}

LogReader::~LogReader() {
}

Status LogReader::Init(const string& tablet_wal_path) {
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    CHECK_EQ(state_, kLogReaderInitialized) << "bad state for Init(): " << state_;
  }
  VLOG(1) << "Reading wal from path:" << tablet_wal_path;

  if (!env_->FileExists(tablet_wal_path)) {
    return Status::IllegalState("Cannot find wal location at", tablet_wal_path);
  }

  VLOG(1) << "Parsing segments from path: " << tablet_wal_path;
  // list existing segment files
  vector<string> log_files;

  RETURN_NOT_OK_PREPEND(env_->GetChildren(tablet_wal_path, &log_files),
                        "Unable to read children from path");

  SegmentSequence read_segments;

  // build a log segment from each file
  for (const string &log_file : log_files) {
    if (HasPrefixString(log_file, FsManager::kWalFileNamePrefix)) {
      string fqp = JoinPathSegments(tablet_wal_path, log_file);
      scoped_refptr<ReadableLogSegment> segment;
      Status s = ReadableLogSegment::Open(env_, fqp, &segment);
      if (s.IsUninitialized()) {
        // This indicates that the segment was created but the writer
        // crashed before the header was successfully written. In this
        // case, we should skip it.
        LOG(WARNING) << "Ignoring log segment " << log_file << " since it was uninitialized "
                     << "(probably left after a prior tablet server crash)";
        continue;
      }

      RETURN_NOT_OK_PREPEND(s, "Unable to open readable log segment");
      DCHECK(segment);
      CHECK(segment->IsInitialized()) << "Uninitialized segment at: " << segment->path();

      if (!segment->HasFooter()) {
        VLOG(1) << "Log segment " << fqp << " was likely left in-progress "
                << "after a previous crash. Will try to rebuild footer by scanning data.";
        RETURN_NOT_OK(segment->RebuildFooterByScanning());
      }

      read_segments.push_back(segment);
    }
  }

  // Sort the segments by sequence number.
  std::sort(read_segments.begin(), read_segments.end(), LogSegmentSeqnoComparator());


  {
    std::lock_guard<simple_spinlock> lock(lock_);

    string previous_seg_path;
    int64_t previous_seg_seqno = -1;
    for (const SegmentSequence::value_type& entry : read_segments) {
      VLOG(1) << " Log Reader Indexed: " << SecureShortDebugString(entry->footer());
      // Check that the log segments are in sequence.
      if (previous_seg_seqno != -1 && entry->header().sequence_number() != previous_seg_seqno + 1) {
        return Status::Corruption(Substitute("Segment sequence numbers are not consecutive. "
            "Previous segment: seqno $0, path $1; Current segment: seqno $2, path $3",
            previous_seg_seqno, previous_seg_path,
            entry->header().sequence_number(), entry->path()));
      }
      previous_seg_seqno = entry->header().sequence_number();
      previous_seg_path = entry->path();
      RETURN_NOT_OK(AppendSegmentUnlocked(entry));
    }

    state_ = kLogReaderReading;
  }
  return Status::OK();
}

Status LogReader::InitEmptyReaderForTests() {
  std::lock_guard<simple_spinlock> lock(lock_);
  state_ = kLogReaderReading;
  return Status::OK();
}


int64_t LogReader::GetMinReplicateIndex() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  int64_t min_remaining_op_idx = -1;

  for (const scoped_refptr<ReadableLogSegment>& segment : segments_) {
    if (!segment->HasFooter()) continue;
    if (!segment->footer().has_min_replicate_index()) continue;
    if (min_remaining_op_idx == -1 ||
        segment->footer().min_replicate_index() < min_remaining_op_idx) {
      min_remaining_op_idx = segment->footer().min_replicate_index();
    }
  }
  return min_remaining_op_idx;
}


scoped_refptr<ReadableLogSegment> LogReader::GetSegmentBySequenceNumber(int64_t seq) const {
  std::lock_guard<simple_spinlock> lock(lock_);
  if (segments_.empty()) {
    return nullptr;
  }

  // We always have a contiguous set of log segments, so we can find the requested
  // segment in our vector by calculating its offset vs the first element.
  int64_t first_seqno = segments_[0]->header().sequence_number();
  int64_t relative = seq - first_seqno;
  if (relative < 0 || relative >= segments_.size()) {
    return nullptr;
  }

  DCHECK_EQ(segments_[relative]->header().sequence_number(), seq);
  return segments_[relative];
}

Status LogReader::ReadBatchUsingIndexEntry(const LogIndexEntry& index_entry,
                                           faststring* tmp_buf,
                                           unique_ptr<LogEntryBatchPB>* batch) const {
  const int64_t index = index_entry.op_id.index();

  scoped_refptr<ReadableLogSegment> segment = GetSegmentBySequenceNumber(
    index_entry.segment_sequence_number);
  if (PREDICT_FALSE(!segment)) {
    return Status::NotFound(Substitute("Segment $0 which contained index $1 has been GCed",
                                       index_entry.segment_sequence_number,
                                       index));
  }

  CHECK_GT(index_entry.offset_in_segment, 0);
  int64_t offset = index_entry.offset_in_segment;
  ScopedLatencyMetric scoped(read_batch_latency_.get());
  EntryHeaderStatus unused_status_detail;
  RETURN_NOT_OK_PREPEND(segment->ReadEntryHeaderAndBatch(&offset, tmp_buf, batch,
                                                         &unused_status_detail),
                        Substitute("Failed to read LogEntry for index $0 from log segment "
                                   "$1 offset $2",
                                   index,
                                   index_entry.segment_sequence_number,
                                   index_entry.offset_in_segment));

  if (bytes_read_) {
    bytes_read_->IncrementBy(segment->entry_header_size() + tmp_buf->length());
    entries_read_->IncrementBy((**batch).entry_size());
  }

  return Status::OK();
}

Status LogReader::ReadReplicatesInRange(int64_t starting_at,
                                        int64_t up_to,
                                        int64_t max_bytes_to_read,
                                        vector<ReplicateMsg*>* replicates) const {
  DCHECK_GT(starting_at, 0);
  DCHECK_GE(up_to, starting_at);
  DCHECK(log_index_) << "Require an index to random-read logs";

  vector<ReplicateMsg*> replicates_tmp;
  ElementDeleter d(&replicates_tmp);
  LogIndexEntry prev_index_entry;

  int64_t total_size = 0;
  bool limit_exceeded = false;
  faststring tmp_buf;
  unique_ptr<LogEntryBatchPB> batch;
  for (int64_t index = starting_at; index <= up_to && !limit_exceeded; index++) {
    LogIndexEntry index_entry;
    RETURN_NOT_OK_PREPEND(log_index_->GetEntry(index, &index_entry),
                          Substitute("Failed to read log index for op $0", index));

    // Since a given LogEntryBatchPB may contain multiple REPLICATE messages,
    // it's likely that this index entry points to the same batch as the previous
    // one. If that's the case, we've already read this REPLICATE and we can
    // skip reading the batch again.
    if (index == starting_at ||
        index_entry.segment_sequence_number != prev_index_entry.segment_sequence_number ||
        index_entry.offset_in_segment != prev_index_entry.offset_in_segment) {
      RETURN_NOT_OK(ReadBatchUsingIndexEntry(index_entry, &tmp_buf, &batch));

      // Sanity-check the property that a batch should only have increasing indexes.
      int64_t prev_index = 0;
      for (int i = 0; i < batch->entry_size(); ++i) {
        LogEntryPB* entry = batch->mutable_entry(i);
        if (!entry->has_replicate()) continue;
        int64_t this_index = entry->replicate().id().index();
        CHECK_GT(this_index, prev_index)
          << "Expected that an entry batch should only include increasing log indexes: "
          << index_entry.ToString()
          << "\nBatch: " << SecureDebugString(*batch);
        prev_index = this_index;
      }
    }

    bool found = false;
    for (int i = 0; i < batch->entry_size(); ++i) {
      LogEntryPB* entry = batch->mutable_entry(i);
      if (!entry->has_replicate()) {
        continue;
      }

      if (entry->replicate().id().index() != index) {
        continue;
      }

      int64_t space_required = entry->replicate().SpaceUsed();
      if (replicates_tmp.empty() ||
          max_bytes_to_read <= 0 ||
          total_size + space_required < max_bytes_to_read) {
        total_size += space_required;
        replicates_tmp.push_back(entry->release_replicate());
      } else {
        limit_exceeded = true;
      }
      found = true;
      break;
    }
    CHECK(found) << "Incorrect index entry didn't yield expected log entry: "
                 << index_entry.ToString();

    prev_index_entry = index_entry;
  }

  replicates->swap(replicates_tmp);
  return Status::OK();
}

Status LogReader::LookupOpId(int64_t op_index, OpId* op_id) const {
  LogIndexEntry index_entry;
  RETURN_NOT_OK_PREPEND(log_index_->GetEntry(op_index, &index_entry),
                        strings::Substitute("Failed to read log index for op $0", op_index));
  *op_id = index_entry.op_id;
  return Status::OK();
}

Status LogReader::GetSegmentsSnapshot(SegmentSequence* segments) const {
  std::lock_guard<simple_spinlock> lock(lock_);
  CHECK_EQ(state_, kLogReaderReading);
  segments->assign(segments_.begin(), segments_.end());
  return Status::OK();
}

Status LogReader::TrimSegmentsUpToAndIncluding(int64_t segment_sequence_number) {
  std::lock_guard<simple_spinlock> lock(lock_);
  CHECK_EQ(state_, kLogReaderReading);
  auto iter = segments_.begin();
  int num_deleted_segments = 0;

  while (iter != segments_.end()) {
    if ((*iter)->header().sequence_number() <= segment_sequence_number) {
      iter = segments_.erase(iter);
      num_deleted_segments++;
      continue;
    }
    break;
  }
  LOG(INFO) << "T " << tablet_id_ << ": removed " << num_deleted_segments
            << " log segments from log reader";
  return Status::OK();
}

void LogReader::UpdateLastSegmentOffset(int64_t readable_to_offset) {
  std::lock_guard<simple_spinlock> lock(lock_);
  CHECK_EQ(state_, kLogReaderReading);
  DCHECK(!segments_.empty());
  // Get the last segment
  ReadableLogSegment* segment = segments_.back().get();
  DCHECK(!segment->HasFooter());
  segment->UpdateReadableToOffset(readable_to_offset);
}

Status LogReader::ReplaceLastSegment(const scoped_refptr<ReadableLogSegment>& segment) {
  // This is used to replace the last segment once we close it properly so it must
  // have a footer.
  DCHECK(segment->HasFooter());

  std::lock_guard<simple_spinlock> lock(lock_);
  CHECK_EQ(state_, kLogReaderReading);
  // Make sure the segment we're replacing has the same sequence number
  CHECK(!segments_.empty());
  CHECK_EQ(segment->header().sequence_number(), segments_.back()->header().sequence_number());
  segments_[segments_.size() - 1] = segment;

  return Status::OK();
}

Status LogReader::AppendSegment(const scoped_refptr<ReadableLogSegment>& segment) {
  DCHECK(segment->IsInitialized());
  if (PREDICT_FALSE(!segment->HasFooter())) {
    RETURN_NOT_OK(segment->RebuildFooterByScanning());
  }
  std::lock_guard<simple_spinlock> lock(lock_);
  return AppendSegmentUnlocked(segment);
}

Status LogReader::AppendSegmentUnlocked(const scoped_refptr<ReadableLogSegment>& segment) {
  DCHECK(segment->IsInitialized());
  DCHECK(segment->HasFooter());

  if (!segments_.empty()) {
    CHECK_EQ(segments_.back()->header().sequence_number() + 1,
             segment->header().sequence_number());
  }
  segments_.push_back(segment);
  return Status::OK();
}

Status LogReader::AppendEmptySegment(const scoped_refptr<ReadableLogSegment>& segment) {
  DCHECK(segment->IsInitialized());
  std::lock_guard<simple_spinlock> lock(lock_);
  CHECK_EQ(state_, kLogReaderReading);
  if (!segments_.empty()) {
    CHECK_EQ(segments_.back()->header().sequence_number() + 1,
             segment->header().sequence_number());
  }
  segments_.push_back(segment);
  return Status::OK();
}

const int LogReader::num_segments() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  return segments_.size();
}

string LogReader::ToString() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  string ret = "Reader's SegmentSequence: \n";
  for (const SegmentSequence::value_type& entry : segments_) {
    ret.append(Substitute("Segment: $0 Footer: $1\n",
                          entry->header().sequence_number(),
                          !entry->HasFooter() ? "NONE" : SecureShortDebugString(entry->footer())));
  }
  return ret;
}

}  // namespace log
}  // namespace kudu
