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

#ifndef KUDU_CONSENSUS_LOG_UTIL_H_
#define KUDU_CONSENSUS_LOG_UTIL_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <gflags/gflags_declare.h>
#include <glog/logging.h>
#include <gtest/gtest_prod.h>

#include "kudu/consensus/log.pb.h"
#include "kudu/consensus/opid.pb.h"
#include "kudu/consensus/ref_counted_replicate.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/util/atomic.h"
#include "kudu/util/env.h"
#include "kudu/util/faststring.h"
#include "kudu/util/slice.h"
#include "kudu/util/status.h"

// Used by other classes, now part of the API.
DECLARE_bool(log_force_fsync_all);

namespace kudu {

class CompressionCodec;

namespace log {

// Each log entry is prefixed by a header. See DecodeEntryHeader()
// implementation for details.
extern const size_t kEntryHeaderSizeV2;

class ReadableLogSegment;

typedef std::vector<std::unique_ptr<LogEntryPB>> LogEntries;

// Options for the State Machine/Write Ahead Log
struct LogOptions {

  // The size of a Log segment
  // Logs will rollover upon reaching this size (default 64 MB)
  size_t segment_size_mb;

  // Whether to call fsync on every call to Append().
  bool force_fsync_all;

  // Whether to fallocate segments before writing to them.
  bool preallocate_segments;

  // Whether the allocation should happen asynchronously.
  bool async_preallocate_segments;

  LogOptions();
};

// A sequence of segments, ordered by increasing sequence number.
typedef std::vector<scoped_refptr<ReadableLogSegment> > SegmentSequence;

// Detailed error codes when decoding entry headers. Used for more fine-grained
// error-handling.
enum class EntryHeaderStatus {
  OK,

  // The entry was just a run of zeros. It's likely we are trying to
  // read from pre-allocated space.
  ALL_ZEROS,

  // The entry checksum didn't match the expected value.
  CRC_MISMATCH,

  // Some other error occurred (eg an IO error while reading)
  OTHER_ERROR
};

// LogEntryReader provides iterator-style access to read the entries
// from an open log segment.
class LogEntryReader {
 public:

  // Construct a LogEntryReader to read from the provided segment.
  // 'seg' must outlive the LogEntryReader.
  explicit LogEntryReader(ReadableLogSegment* seg);

  ~LogEntryReader();

  // Read the next entry from the log, replacing the contents of 'entry'.
  //
  // When there are no more entries to read, returns Status::EndOfFile().
  Status ReadNextEntry(std::unique_ptr<LogEntryPB>* entry);

  // Return the offset of the next entry to be read from the file.
  int64_t offset() const {
    return offset_;
  }

  // Return the offset at which this reader will stop reading.
  int64_t read_up_to_offset() const {
    return read_up_to_;
  }

 private:
  friend class ReadableLogSegment;

  // Handle an error reading an entry.
  Status HandleReadError(const Status& s, EntryHeaderStatus status_detail) const;

  // Format a nice error message to report on a corruption in a log file.
  Status MakeCorruptionStatus(const Status& status) const;

  // The segment being read.
  ReadableLogSegment* seg_;

  // The last several entries which were successfully read.
  struct RecentEntry {
    int64_t offset;
    LogEntryTypePB type;
    consensus::OpId op_id;
  };
  std::deque<RecentEntry> recent_entries_;
  static const int kNumRecentEntries = 4;

  // Entries which have been read from the file and not yet returned to
  // the caller.
  std::deque<std::unique_ptr<LogEntryPB>> pending_entries_;

  // The total number of log entry batches read from the file.
  int64_t num_batches_read_;

  // The total number of LogEntryPBs read from the file.
  int64_t num_entries_read_;

  // The offset of the next entry to be read.
  int64_t offset_;

  // The offset at which this reader will stop reading entries.
  int64_t read_up_to_;

  // Temporary buffer used for deserialization.
  faststring tmp_buf_;

  DISALLOW_COPY_AND_ASSIGN(LogEntryReader);
};

// A segment of the log can either be a ReadableLogSegment (for replay and
// consensus catch-up) or a WritableLogSegment (where the Log actually stores
// state). LogSegments have a maximum size defined in LogOptions (set from the
// log_segment_size_mb flag, which defaults to 64). Upon reaching this size
// segments are rolled over and the Log continues in a new segment.

// A readable log segment for recovery and follower catch-up.
class ReadableLogSegment : public RefCountedThreadSafe<ReadableLogSegment> {
 public:
  // Factory method to construct a ReadableLogSegment from a file on the FS.
  static Status Open(Env* env,
                     const std::string& path,
                     scoped_refptr<ReadableLogSegment>* segment);

  // Build a readable segment to read entries from the provided path.
  ReadableLogSegment(std::string path,
                     std::shared_ptr<RandomAccessFile> readable_file);

  // Initialize the ReadableLogSegment.
  // This initializer provides methods for avoiding disk IO when creating a
  // ReadableLogSegment for the current WritableLogSegment, i.e. for reading
  // the log entries in the same segment that is currently being written to.
  Status Init(const LogSegmentHeaderPB& header,
              int64_t first_entry_offset);

  // Initialize the ReadableLogSegment.
  // This initializer provides methods for avoiding disk IO when creating a
  // ReadableLogSegment from a WritableLogSegment (i.e. for log rolling).
  Status Init(const LogSegmentHeaderPB& header,
              const LogSegmentFooterPB& footer,
              int64_t first_entry_offset);

  // Initialize the ReadableLogSegment.
  // This initializer will parse the log segment header and footer.
  // Note: This returns Status and may fail.
  Status Init();

  // Reads all entries of the provided segment & adds them the 'entries' vector.
  // The 'entries' vector owns the read entries.
  //
  // If the log is corrupted (i.e. the returned 'Status' is 'Corruption') all
  // the log entries read up to the corrupted one are returned in the 'entries'
  // vector.
  Status ReadEntries(LogEntries* entries);

  // Rebuilds this segment's footer by scanning its entries.
  // This is an expensive operation as it reads and parses the whole segment
  // so it should be only used in the case of a crash, where the footer is
  // missing because we didn't have the time to write it out.
  Status RebuildFooterByScanning();

  bool IsInitialized() const {
    return is_initialized_;
  }

  // Returns the parent directory where log segments are stored.
  const std::string &path() const {
    return path_;
  }

  const LogSegmentHeaderPB& header() const {
    DCHECK(header_.IsInitialized());
    return header_;
  }

  // Indicates whether this segment has a footer.
  //
  // Segments that were properly closed, e.g. because they were rolled over,
  // will have properly written footers. On the other hand if there was a
  // crash and the segment was not closed properly the footer will be missing.
  // In this case calling ReadEntries() will rebuild the footer.
  bool HasFooter() const {
    return footer_.IsInitialized();
  }

  // Returns this log segment's footer.
  //
  // If HasFooter() returns false this cannot be called.
  const LogSegmentFooterPB& footer() const {
    DCHECK(IsInitialized());
    CHECK(HasFooter());
    return footer_;
  }

  const std::shared_ptr<RandomAccessFile> readable_file() const {
    return readable_file_;
  }

  const int64_t file_size() const {
    return file_size_.Load();
  }

  const int64_t first_entry_offset() const {
    return first_entry_offset_;
  }

  // Returns the full size of the file, if the segment is closed and has
  // a footer, or the offset where the last written, non corrupt entry
  // ends.
  const int64_t readable_up_to() const;

  // Return the expected length of entry headers in this log segment.
  // Versions of Kudu older than 1.3 used a different log entry header format.
  size_t entry_header_size() const;

 private:
  friend class RefCountedThreadSafe<ReadableLogSegment>;
  friend class LogEntryReader;
  friend class LogReader;
  FRIEND_TEST(LogTest, TestWriteAndReadToAndFromInProgressSegment);

  struct EntryHeader {
    // The length of the batch data (uncompressed)
    uint32_t msg_length;

    // The compressed length of the entry. If compression is disabled,
    // equal to msg_length.
    uint32_t msg_length_compressed;

    // The CRC32C of the batch data.
    // If compression is enabled, this is the checksum of the compressed data.
    uint32_t msg_crc;

    // The CRC32C of this EntryHeader.
    uint32_t header_crc;
  };

  ~ReadableLogSegment() {}

  // Helper functions called by Init().

  Status ReadFileSize();

  Status InitCompressionCodec();

  // Read the log file magic and header protobuf into 'header_'. Sets 'first_entry_offset_'
  // to indicate the start of the actual log data.
  //
  // Returns Uninitialized() if the file appears to be preallocated but never
  // written.
  Status ReadHeader();

  // Read the magic and header length from the top of the file, returning
  // the header length in 'len'.
  //
  // Returns Uninitialized() if the file appears to be preallocated but never
  // written.
  Status ReadHeaderMagicAndHeaderLength(uint32_t *len);

  // Parse the magic and the PB-header length prefix from 'data'.
  // In the case that 'data' is all '\0' bytes, indicating a preallocated
  // but never-written segment, returns Status::Uninitialized().
  Status ParseHeaderMagicAndHeaderLength(const Slice &data, uint32_t *parsed_len);

  Status ReadFooter();

  Status ReadFooterMagicAndFooterLength(uint32_t *len);

  Status ParseFooterMagicAndFooterLength(const Slice &data, uint32_t *parsed_len);

  // Starting at 'offset', read the rest of the log file, looking for any
  // valid log entry headers. If any are found, sets *has_valid_entries to true.
  //
  // Returns a bad Status only in the case that some IO error occurred reading the
  // file.
  Status ScanForValidEntryHeaders(int64_t offset, bool* has_valid_entries);

  // Read an entry header and its associated batch at the given offset.
  // If successful, updates '*offset' to point to the next batch
  // in the file.
  //
  // If unsuccessful, '*offset' is not updated, and *status_detail will be updated
  // to indicate the cause of the error.
  Status ReadEntryHeaderAndBatch(int64_t* offset, faststring* tmp_buf,
                                 std::unique_ptr<LogEntryBatchPB>* batch,
                                 EntryHeaderStatus* status_detail);

  // Reads a log entry header from the segment.
  //
  // Also increments the passed offset* by the length of the entry on successful
  // read.
  Status ReadEntryHeader(int64_t *offset, EntryHeader* header,
                         EntryHeaderStatus* status_detail);

  // Decode a log entry header from the given slice. The header length is
  // determined by 'entry_header_size()'.
  // Returns true if successful, false if corrupt.
  //
  // NOTE: this is performance-critical since it is used by ScanForValidEntryHeaders
  // and thus returns an enum instead of Status.
  EntryHeaderStatus DecodeEntryHeader(const Slice& data, EntryHeader* header);

  // Reads a log entry batch from the provided readable segment, which gets decoded
  // into 'entry_batch' and increments 'offset' by the batch's length.
  Status ReadEntryBatch(int64_t* offset,
                        const EntryHeader& header,
                        faststring* tmp_buf,
                        std::unique_ptr<LogEntryBatchPB>* entry_batch);

  void UpdateReadableToOffset(int64_t readable_to_offset);

  const std::string path_;

  // The size of the readable file.
  // This is set by Init(). In the case of a log being written to,
  // this may be increased by UpdateReadableToOffset()
  AtomicInt<int64_t> file_size_;

  // The offset up to which we can read the file.
  // For already written segments this is fixed and equal to the file size
  // but for the segments currently written to this is the offset up to which
  // we can read without the fear of reading garbage/zeros.
  // This is atomic because the Log thread might be updating the segment's readable
  // offset while an async reader is reading the segment's entries.
  // is reading it.
  AtomicInt<int64_t> readable_to_offset_;

  // a readable file for a log segment (used on replay)
  const std::shared_ptr<RandomAccessFile> readable_file_;

  // Compression codec used to decompress entries in this file.
  const CompressionCodec* codec_;

  bool is_initialized_;

  LogSegmentHeaderPB header_;

  LogSegmentFooterPB footer_;

  // True if the footer was rebuilt, rather than actually found on disk.
  bool footer_was_rebuilt_;

  // the offset of the first entry in the log
  int64_t first_entry_offset_;

  DISALLOW_COPY_AND_ASSIGN(ReadableLogSegment);
};

// A writable log segment where state data is stored.
class WritableLogSegment {
 public:
  WritableLogSegment(std::string path,
                     std::shared_ptr<WritableFile> writable_file);

  // Opens the segment by writing the header.
  Status WriteHeaderAndOpen(const LogSegmentHeaderPB& new_header);

  // Closes the segment by writing the footer and then actually closing the
  // underlying WritableFile.
  Status WriteFooterAndClose(const LogSegmentFooterPB& footer);

  bool IsClosed() {
    return IsHeaderWritten() && IsFooterWritten();
  }

  int64_t Size() const {
    return writable_file_->Size();
  }

  // Appends the provided batch of data, including a header
  // and checksum. If 'codec' is not NULL, compresses the batch.
  // Makes sure that the log segment has not been closed.
  // Write a compressed entry to the log.
  Status WriteEntryBatch(const Slice& data, const CompressionCodec* codec);

  // Makes sure the I/O buffers in the underlying writable file are flushed.
  Status Sync() {
    return writable_file_->Sync();
  }

  // Returns true if the segment header has already been written to disk.
  bool IsHeaderWritten() const {
    return is_header_written_;
  }

  const LogSegmentHeaderPB& header() const {
    DCHECK(IsHeaderWritten());
    return header_;
  }

  bool IsFooterWritten() const {
    return is_footer_written_;
  }

  const LogSegmentFooterPB& footer() const {
    DCHECK(IsFooterWritten());
    return footer_;
  }

  // Returns the parent directory where log segments are stored.
  const std::string &path() const {
    return path_;
  }

  const int64_t first_entry_offset() const {
    return first_entry_offset_;
  }

  const int64_t written_offset() const {
    return written_offset_;
  }

 private:

  const std::shared_ptr<WritableFile>& writable_file() const {
    return writable_file_;
  }

  // The path to the log file.
  const std::string path_;

  // The writable file to which this LogSegment will be written.
  const std::shared_ptr<WritableFile> writable_file_;

  bool is_header_written_;

  bool is_footer_written_;

  LogSegmentHeaderPB header_;

  LogSegmentFooterPB footer_;

  // the offset of the first entry in the log
  int64_t first_entry_offset_;

  // The offset where the last written entry ends.
  int64_t written_offset_;

  // Buffer used for output when compressing.
  faststring compress_buf_;

  DISALLOW_COPY_AND_ASSIGN(WritableLogSegment);
};

// Return a newly created batch that contains the pre-allocated
// ReplicateMsgs in 'msgs'.
std::unique_ptr<LogEntryBatchPB> CreateBatchFromAllocatedOperations(
    const std::vector<consensus::ReplicateRefPtr>& msgs);

// Checks if 'fname' is a correctly formatted name of log segment file.
bool IsLogFileName(const std::string& fname);

// Update 'footer' to reflect the given REPLICATE message 'entry_pb'.
// In particular, updates the min/max seen replicate OpID.
void UpdateFooterForReplicateEntry(
    const LogEntryPB& entry_pb, LogSegmentFooterPB* footer);

}  // namespace log
}  // namespace kudu

#endif /* KUDU_CONSENSUS_LOG_UTIL_H_ */
