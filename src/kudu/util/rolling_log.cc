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

#include "kudu/util/rolling_log.h"

#include <unistd.h>

#include <ctime>
#include <iomanip>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include <gflags/gflags.h>
#include <gflags/gflags_declare.h>
#include <glog/logging.h>
#include <zlib.h>

#include "kudu/gutil/strings/numbers.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/walltime.h"
#include "kudu/util/env.h"
#include "kudu/util/env_util.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/path_util.h"
#include "kudu/util/slice.h"
#include "kudu/util/user.h"

using std::ostringstream;
using std::setw;
using std::string;
using std::unique_ptr;
using strings::Substitute;

static const int kDefaultRollThresholdBytes = 64 * 1024 * 1024; // 64MB

DECLARE_int32(max_log_files);

namespace kudu {

RollingLog::RollingLog(Env* env, string log_dir, string log_name)
    : env_(env),
      log_dir_(std::move(log_dir)),
      log_name_(std::move(log_name)),
      roll_threshold_bytes_(kDefaultRollThresholdBytes),
      max_num_segments_(FLAGS_max_log_files),
      compress_after_close_(true) {}

RollingLog::~RollingLog() {
  WARN_NOT_OK(Close(), "Unable to close RollingLog");
}

void RollingLog::SetRollThresholdBytes(int64_t size) {
  CHECK_GT(size, 0);
  roll_threshold_bytes_ = size;
}

void RollingLog::SetMaxNumSegments(int num_segments) {
  CHECK_GT(num_segments, 0);
  max_num_segments_ = num_segments;
}

void RollingLog::SetCompressionEnabled(bool compress) {
  compress_after_close_ = compress;
}

namespace {

string HostnameOrUnknown() {
  string hostname;
  Status s = GetHostname(&hostname);
  if (!s.ok()) {
    return "unknown_host";
  }
  return hostname;
}

string UsernameOrUnknown() {
  string user_name;
  Status s = GetLoggedInUser(&user_name);
  if (!s.ok()) {
    return "unknown_user";
  }
  return user_name;
}

string FormattedTimestamp() {
  // Implementation cribbed from glog/logging.cc
  time_t time = static_cast<time_t>(WallTime_Now());
  struct ::tm tm_time;
  localtime_r(&time, &tm_time);

  ostringstream str;
  str.fill('0');
  str << 1900+tm_time.tm_year
      << setw(2) << 1+tm_time.tm_mon
      << setw(2) << tm_time.tm_mday
      << '-'
      << setw(2) << tm_time.tm_hour
      << setw(2) << tm_time.tm_min
      << setw(2) << tm_time.tm_sec;
  return str.str();
}

} // anonymous namespace

string RollingLog::GetLogFileName(int sequence) const {
  return Substitute("$0.$1.$2.$3.$4.$5.$6",
                    google::ProgramInvocationShortName(),
                    HostnameOrUnknown(),
                    UsernameOrUnknown(),
                    log_name_,
                    FormattedTimestamp(),
                    sequence,
                    getpid());
}

string RollingLog::GetLogFilePattern() const {
  return Substitute("$0.$1.$2.$3.$4.$5.$6",
                    google::ProgramInvocationShortName(),
                    HostnameOrUnknown(),
                    UsernameOrUnknown(),
                    log_name_,
                    /* any timestamp */'*',
                    /* any sequence number */'*',
                    /* any pid */'*');
}

Status RollingLog::Open() {
  CHECK(!file_);

  for (int sequence = 0; ; sequence++) {

    string path = JoinPathSegments(log_dir_, GetLogFileName(sequence));
    // Don't reuse an existing path if there is already a log
    // or a compressed log with the same name.
    if (env_->FileExists(path) ||
        env_->FileExists(path + ".gz")) {
      continue;
    }

    WritableFileOptions opts;
    // Logs aren't worth the performance cost of durability.
    opts.sync_on_close = false;
    opts.mode = Env::CREATE_NON_EXISTING;

    RETURN_NOT_OK(env_->NewWritableFile(opts, path, &file_));

    VLOG(1) << "Rolled " << log_name_ << " log to new file: " << path;
    break;
  }
  return Status::OK();
}

Status RollingLog::Close() {
  if (!file_) {
    return Status::OK();
  }
  string path = file_->filename();
  RETURN_NOT_OK_PREPEND(file_->Close(),
                        Substitute("Unable to close $0", path));
  file_.reset();
  if (compress_after_close_) {
    WARN_NOT_OK(CompressFile(path), "Unable to compress old log file");
  }
  auto glob = JoinPathSegments(log_dir_, GetLogFilePattern());
  WARN_NOT_OK(env_util::DeleteExcessFilesByPattern(env_, glob, max_num_segments_),
              Substitute("failed to delete old $0 log files", log_name_));
  return Status::OK();
}

Status RollingLog::Append(StringPiece s) {
  if (!file_) {
    RETURN_NOT_OK_PREPEND(Open(), "Unable to open log");
  }

  RETURN_NOT_OK(file_->Append(s));
  if (file_->Size() > roll_threshold_bytes_) {
    RETURN_NOT_OK_PREPEND(Close(), "Unable to roll log");
    roll_count_++;
    RETURN_NOT_OK_PREPEND(Open(), "Unable to roll log");
  }
  return Status::OK();
}

namespace {

Status GzClose(gzFile f) {
  int err = gzclose(f);
  switch (err) {
    case Z_OK:
      return Status::OK();
    case Z_STREAM_ERROR:
      return Status::InvalidArgument("Stream not valid");
    case Z_ERRNO:
      return Status::IOError("IO Error closing stream");
    case Z_MEM_ERROR:
      return Status::RuntimeError("Out of memory");
    case Z_BUF_ERROR:
      return Status::IOError("read ended in the middle of a stream");
    default:
      return Status::IOError("Unknown zlib error", SimpleItoa(err));
  }
}

class ScopedGzipCloser {
 public:
  explicit ScopedGzipCloser(gzFile f)
    : file_(f) {
  }

  ~ScopedGzipCloser() {
    if (file_) {
      WARN_NOT_OK(GzClose(file_), "Unable to close gzip stream");
    }
  }

  void Cancel() {
    file_ = nullptr;
  }

 private:
  gzFile file_;
};
} // anonymous namespace

// We implement CompressFile() manually using zlib APIs rather than forking
// out to '/bin/gzip' since fork() can be expensive on processes that use a large
// amount of memory. During the time of the fork, other threads could end up
// blocked. Implementing it using the zlib stream APIs isn't too much code
// and is less likely to be problematic.
Status RollingLog::CompressFile(const std::string& path) const {
  unique_ptr<SequentialFile> in_file;
  RETURN_NOT_OK_PREPEND(env_->NewSequentialFile(path, &in_file),
                        "Unable to open input file to compress");

  string gz_path = path + ".gz";
  gzFile gzf = gzopen(gz_path.c_str(), "w");
  if (!gzf) {
    return Status::IOError("Unable to open gzip stream");
  }

  ScopedGzipCloser closer(gzf);

  // Loop reading data from the input file and writing to the gzip stream.
  uint8_t buf[32 * 1024];
  while (true) {
    Slice result(buf, arraysize(buf));
    RETURN_NOT_OK_PREPEND(in_file->Read(&result),
                          "Unable to read from gzip input");
    if (result.size() == 0) {
      break;
    }
    int n = gzwrite(gzf, result.data(), result.size());
    if (n == 0) {
      int errnum;
      return Status::IOError("Unable to write to gzip output",
                             gzerror(gzf, &errnum));
    }
  }
  closer.Cancel();
  RETURN_NOT_OK_PREPEND(GzClose(gzf),
                        "Unable to close gzip output");

  WARN_NOT_OK(env_->DeleteFile(path),
              "Unable to delete gzip input file after compression");
  return Status::OK();
}

} // namespace kudu
