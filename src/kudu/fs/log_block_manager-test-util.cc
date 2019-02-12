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

#include "kudu/fs/log_block_manager-test-util.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gflags/gflags_declare.h>
#include <glog/logging.h>

#include "kudu/fs/block_id.h"
#include "kudu/fs/fs.pb.h"
#include "kudu/fs/log_block_manager.h"
#include "kudu/gutil/integral_types.h"
#include "kudu/gutil/strings/strcat.h"
#include "kudu/gutil/strings/strip.h"
#include "kudu/util/env.h"
#include "kudu/util/path_util.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/slice.h"
#include "kudu/util/status.h"

DECLARE_uint64(log_container_max_size);

namespace kudu {
namespace fs {

using pb_util::WritablePBContainerFile;
using std::shared_ptr;
using std::string;
using std::vector;
using std::unique_ptr;
using std::unordered_map;

LBMCorruptor::LBMCorruptor(Env* env, vector<string> data_dirs, uint32_t rand_seed)
    : env_(env),
      data_dirs_(std::move(data_dirs)),
      rand_(rand_seed) {
  CHECK_GT(data_dirs_.size(), 0);
}

Status LBMCorruptor::Init() {
  vector<Container> all_containers;
  vector<Container> full_containers;

  for (const auto& dd : data_dirs_) {
    vector<string> dd_files;
    unordered_map<string, Container> containers_by_name;
    RETURN_NOT_OK(env_->GetChildren(dd, &dd_files));
    for (const auto& f : dd_files) {
      // As we iterate over each file in the data directory, keep track of data
      // and metadata files, so that only containers with both will be included.
      string stripped;
      if (TryStripSuffixString(
          f, LogBlockManager::kContainerDataFileSuffix, &stripped)) {
        containers_by_name[stripped].name = stripped;
        containers_by_name[stripped].data_filename = JoinPathSegments(dd, f);
      } else if (TryStripSuffixString(
          f, LogBlockManager::kContainerMetadataFileSuffix, &stripped)) {
        containers_by_name[stripped].name = stripped;
        containers_by_name[stripped].metadata_filename = JoinPathSegments(dd, f);
      }
    }

    for (const auto& e : containers_by_name) {
      // Only include the container if both of its files were present.
      if (!e.second.data_filename.empty() &&
          !e.second.metadata_filename.empty()) {
        all_containers.push_back(e.second);

        // File size is an imprecise proxy for whether a container is full, but
        // it should be good enough.
        uint64_t data_file_size;
        RETURN_NOT_OK(env_->GetFileSize(e.second.data_filename,
                                        &data_file_size));
        if (data_file_size >= FLAGS_log_container_max_size) {
          full_containers.push_back(e.second);
        }
      }
    }
  }

  all_containers_ = std::move(all_containers);
  full_containers_ = std::move(full_containers);
  return Status::OK();
}

Status LBMCorruptor::PreallocateFullContainer() {
  const int kPreallocateBytes = 16 * 1024;
  const Container* c;
  RETURN_NOT_OK(GetRandomContainer(FULL, &c));

  // Pick one of the preallocation modes at random; both are recoverable.
  RWFile::PreAllocateMode mode;
  int r = rand_.Uniform(2);
  if (r == 0) {
    mode = RWFile::CHANGE_FILE_SIZE;
  } else {
    CHECK_EQ(r, 1);
    mode = RWFile::DONT_CHANGE_FILE_SIZE;
  }

  unique_ptr<RWFile> data_file;
  RWFileOptions opts;
  opts.mode = Env::OPEN_EXISTING;
  RETURN_NOT_OK(env_->NewRWFile(opts, c->data_filename, &data_file));
  int64_t initial_size;
  RETURN_NOT_OK(PreallocateForBlock(data_file.get(), mode,
                                    kPreallocateBytes, &initial_size));
  if (mode == RWFile::DONT_CHANGE_FILE_SIZE) {
    // Some older versions of ext4 (such as on el6) will not truncate unwritten
    // preallocated space that extends beyond the file size. Let's help them
    // out by writing a single byte into that space.
    RETURN_NOT_OK(data_file->Write(initial_size, "a"));
  }

  RETURN_NOT_OK(data_file->Close());

  LOG(INFO) << "Preallocated full container " << c->name;
  return Status::OK();
}

Status LBMCorruptor::AddUnpunchedBlockToFullContainer() {
  const Container* c;
  RETURN_NOT_OK(GetRandomContainer(FULL, &c));

  uint64_t fs_block_size;
  RETURN_NOT_OK(env_->GetBlockSize(c->data_filename, &fs_block_size));

  // "Write" out the block by growing the data file by some random amount.
  //
  // Must be non-zero length, otherwise preallocation will fail.
  unique_ptr<RWFile> data_file;
  RWFileOptions opts;
  opts.mode = Env::OPEN_EXISTING;
  RETURN_NOT_OK(env_->NewRWFile(opts, c->data_filename, &data_file));
  int64_t block_length = (rand_.Uniform(16) + 1) * fs_block_size;
  int64_t initial_data_size;
  RETURN_NOT_OK(PreallocateForBlock(data_file.get(), RWFile::CHANGE_FILE_SIZE,
                                    block_length, &initial_data_size));
  RETURN_NOT_OK(data_file->Close());

  // Having written out the block, write both CREATE and DELETE metadata
  // records for it.
  unique_ptr<WritablePBContainerFile> metadata_writer;
  RETURN_NOT_OK(OpenMetadataWriter(*c, &metadata_writer));
  BlockId block_id(rand_.Next64());
  RETURN_NOT_OK(AppendCreateRecord(metadata_writer.get(), block_id,
                                   initial_data_size, block_length));
  RETURN_NOT_OK(AppendDeleteRecord(metadata_writer.get(), block_id));

  LOG(INFO) << "Added unpunched block to full container " << c->name;
  return metadata_writer->Close();
}

Status LBMCorruptor::CreateIncompleteContainer() {
  unique_ptr<RWFile> data_file;
  unique_ptr<RWFile> metadata_file;
  string unsuffixed_path = JoinPathSegments(GetRandomDataDir(),
                                            oid_generator_.Next());
  string data_fname = StrCat(
      unsuffixed_path, LogBlockManager::kContainerDataFileSuffix);
  string metadata_fname = StrCat(
      unsuffixed_path, LogBlockManager::kContainerMetadataFileSuffix);

  // Create an incomplete container. Kinds of incomplete containers:
  //
  // 1. Empty data file but no metadata file.
  // 2. No data file but metadata file exists (and is up to a certain size).
  // 3. Empty data file and metadata file exists (and is up to a certain size).
  int r = rand_.Uniform(3);
  if (r == 0) {
    RETURN_NOT_OK(env_->NewRWFile(data_fname, &data_file));
  } else if (r == 1) {
    RETURN_NOT_OK(env_->NewRWFile(metadata_fname, &data_file));
  } else {
    CHECK_EQ(r, 2);
    RETURN_NOT_OK(env_->NewRWFile(data_fname, &data_file));
    RETURN_NOT_OK(env_->NewRWFile(metadata_fname, &data_file));
  }

  if (data_file) {
    RETURN_NOT_OK(data_file->Close());
  }

  if (metadata_file) {
    int md_length = rand_.Uniform(pb_util::kPBContainerMinimumValidLength);
    RETURN_NOT_OK(metadata_file->Truncate(md_length));
    RETURN_NOT_OK(metadata_file->Close());
  }

  LOG(INFO) << "Created incomplete container " << unsuffixed_path;
  return Status::OK();
}

Status LBMCorruptor::AddMalformedRecordToContainer() {
  const int kBlockSize = 16 * 1024;
  const Container* c;
  RETURN_NOT_OK(GetRandomContainer(ANY, &c));

  // Ensure the container's data file has enough space for the new block. We're
  // not going to fill that space, but this ensures that the block's record
  // isn't considered malformed only because it stretches past the end of the
  // data file.
  int64_t initial_data_size;
  {
    unique_ptr<RWFile> data_file;
    RWFileOptions opts;
    opts.mode = Env::OPEN_EXISTING;
    RETURN_NOT_OK(env_->NewRWFile(opts, c->data_filename, &data_file));
    RETURN_NOT_OK(PreallocateForBlock(data_file.get(), RWFile::CHANGE_FILE_SIZE,
                                      kBlockSize, &initial_data_size));
    RETURN_NOT_OK(data_file->Close());
  }

  // Create a good record.
  BlockId block_id(rand_.Next64());
  BlockRecordPB record;
  block_id.CopyToPB(record.mutable_block_id());
  record.set_op_type(CREATE);
  record.set_offset(initial_data_size);
  record.set_length(kBlockSize);
  record.set_timestamp_us(0);

  unique_ptr<WritablePBContainerFile> metadata_writer;
  RETURN_NOT_OK(OpenMetadataWriter(*c, &metadata_writer));

  // Corrupt the record in some way. Kinds of malformed records (as per the
  // malformed record checking code in log_block_manager.cc):
  //
  // 0. No block offset.
  // 1. No block length.
  // 2. Negative block offset.
  // 3. Negative block length.
  // 4. Offset + length > data file size.
  // 5. Two CREATEs for same block ID.
  // 6. DELETE without first matching CREATE.
  // 7. Unrecognized op type.
  int r = rand_.Uniform(8);
  if (r == 0) {
    record.clear_offset();
  } else if (r == 1) {
    record.clear_length();
  } else if (r == 2) {
    record.set_offset(-1);
  } else if (r == 3) {
    record.set_length(-1);
  } else if (r == 4) {
    record.set_offset(kint64max / 2);
  } else if (r == 5) {
    RETURN_NOT_OK(metadata_writer->Append(record));
  } else if (r == 6) {
    record.clear_offset();
    record.clear_length();
    record.set_op_type(DELETE);
  } else {
    CHECK_EQ(r, 7);
    record.set_op_type(UNKNOWN);
  }

  LOG(INFO) << "Added malformed record to container " << c->name;
  return metadata_writer->Append(record);
}

Status LBMCorruptor::AddMisalignedBlockToContainer() {
  const Container* c;
  RETURN_NOT_OK(GetRandomContainer(ANY, &c));

  uint64_t fs_block_size;
  RETURN_NOT_OK(env_->GetBlockSize(c->data_filename, &fs_block_size));

  unique_ptr<RWFile> data_file;
  RWFileOptions opts;
  opts.mode = Env::OPEN_EXISTING;
  RETURN_NOT_OK(env_->NewRWFile(opts, c->data_filename, &data_file));
  uint64_t initial_data_size;
  RETURN_NOT_OK(data_file->Size(&initial_data_size));

  // Pick a random offset beyond the end of the file to place the new block,
  // ensuring that the offset isn't aligned with the filesystem block size.
  //
  // In accordance with KUDU-1793 (which sparked the entire concept of
  // misaligned blocks in the first place), misaligned blocks may not intrude
  // on the aligned space of the blocks that came before them. To avoid having
  // to read the container's records just to corrupt it, we'll arbitrarily add
  // a fs_block_size gap before this misaligned block, to ensure that it
  // doesn't violate the previous block's alignment.
  uint64_t block_offset =
      initial_data_size + fs_block_size + rand_.Uniform(fs_block_size);
  if (block_offset % fs_block_size == 0) {
    block_offset++;
  }

  // Ensure the file is preallocated at least up to the offset, in case we
  // decide to write a zero-length block to the end of it.
  uint64_t length_beyond_eof = block_offset - initial_data_size;
  if (length_beyond_eof > 0) {
    RETURN_NOT_OK(data_file->PreAllocate(initial_data_size, length_beyond_eof,
                                         RWFile::CHANGE_FILE_SIZE));
  }

  // Populate the block with repeated sequences of its id so that readers who
  // wish to verify its contents can do so easily. To avoid a truncated
  // sequence at the end of the block, we also ensure that the block's length
  // is a multiple of the id's type.
  BlockId block_id(rand_.Next64());
  uint64_t raw_block_id = block_id.id();
  uint64_t block_length = rand_.Uniform(fs_block_size * 4);
  block_length -= block_length % sizeof(raw_block_id);
  uint8_t data[block_length];
  for (int i = 0; i < ARRAYSIZE(data); i += sizeof(raw_block_id)) {
    memcpy(&data[i], &raw_block_id, sizeof(raw_block_id));
  }
  RETURN_NOT_OK(data_file->Write(block_offset, Slice(data, ARRAYSIZE(data))));
  RETURN_NOT_OK(data_file->Close());

  // Having written out the block, write a corresponding metadata record.
  unique_ptr<WritablePBContainerFile> metadata_writer;
  RETURN_NOT_OK(OpenMetadataWriter(*c, &metadata_writer));
  RETURN_NOT_OK(AppendCreateRecord(metadata_writer.get(), block_id,
                                   block_offset, block_length));

  LOG(INFO) << "Added misaligned block to container " << c->name;
  return metadata_writer->Close();
}

Status LBMCorruptor::AddPartialRecordToContainer() {
  const Container* c;
  RETURN_NOT_OK(GetRandomContainer(ANY, &c));

  unique_ptr<WritablePBContainerFile> metadata_writer;
  RETURN_NOT_OK(OpenMetadataWriter(*c, &metadata_writer));

  // Add a new good record to the container.
  RETURN_NOT_OK(AppendCreateRecord(metadata_writer.get(),
                                   BlockId(rand_.Next64()),
                                   0, 0));

  // Corrupt the record by truncating one byte off the end of it.
  {
    RWFileOptions opts;
    opts.mode = Env::OPEN_EXISTING;
    unique_ptr<RWFile> metadata_file;
    RETURN_NOT_OK(env_->NewRWFile(opts, c->metadata_filename, &metadata_file));
    uint64_t initial_metadata_size;
    RETURN_NOT_OK(metadata_file->Size(&initial_metadata_size));
    RETURN_NOT_OK(metadata_file->Truncate(initial_metadata_size - 1));
  }

  // Once a container has a partial record, it cannot be further corrupted by
  // the corruptor.

  // Make a local copy of the container's name; erase() below will free it.
  string container_name = c->name;

  auto remove_matching_container = [&](const Container& e) {
    return container_name == e.name;
  };
  all_containers_.erase(std::remove_if(all_containers_.begin(),
                                       all_containers_.end(),
                                       remove_matching_container),
                        all_containers_.end());
  full_containers_.erase(std::remove_if(full_containers_.begin(),
                                        full_containers_.end(),
                                        remove_matching_container),
                        full_containers_.end());

  LOG(INFO) << "Added partial record to container " << container_name;
  return Status::OK();
}

Status LBMCorruptor::InjectRandomNonFatalInconsistency() {
  while (true) {
    int r = rand_.Uniform(5);
    switch (r) {
      case 0:
        return AddMisalignedBlockToContainer();
      case 1:
        return CreateIncompleteContainer();
      case 2:
        if (full_containers_.empty()) {
          // Loop and try a different operation.
          break;
        }
        return PreallocateFullContainer();
      case 3:
        if (full_containers_.empty()) {
          // Loop and try a different operation.
          break;
        }
        return AddUnpunchedBlockToFullContainer();
      case 4:
        return AddPartialRecordToContainer();
      default:
        LOG(FATAL) << "Unexpected value " << r;
    }
  }
}

Status LBMCorruptor::OpenMetadataWriter(
    const Container& container,
    unique_ptr<WritablePBContainerFile>* writer) {
  RWFileOptions opts;
  opts.mode = Env::OPEN_EXISTING;
  unique_ptr<RWFile> metadata_file;
  RETURN_NOT_OK(env_->NewRWFile(opts,
                                container.metadata_filename,
                                &metadata_file));
  unique_ptr<WritablePBContainerFile> local_writer(
      new WritablePBContainerFile(shared_ptr<RWFile>(metadata_file.release())));
  RETURN_NOT_OK(local_writer->OpenExisting());

  *writer = std::move(local_writer);
  return Status::OK();
}

Status LBMCorruptor::AppendCreateRecord(WritablePBContainerFile* writer,
                                        BlockId block_id,
                                        int64_t block_offset,
                                        int64_t block_length) {
  BlockRecordPB record;
  block_id.CopyToPB(record.mutable_block_id());
  record.set_op_type(CREATE);
  record.set_offset(block_offset);
  record.set_length(block_length);
  record.set_timestamp_us(0); // has no effect
  return writer->Append(record);
}

Status LBMCorruptor::AppendDeleteRecord(WritablePBContainerFile* writer,
                                        BlockId block_id) {
  BlockRecordPB record;
  block_id.CopyToPB(record.mutable_block_id());
  record.set_op_type(DELETE);
  record.set_timestamp_us(0); // has no effect
  return writer->Append(record);
}

Status LBMCorruptor::PreallocateForBlock(RWFile* data_file,
                                         RWFile::PreAllocateMode mode,
                                         int64_t block_length,
                                         int64_t* old_data_file_size) {
  uint64_t initial_size;
  RETURN_NOT_OK(data_file->Size(&initial_size));
  RETURN_NOT_OK(data_file->PreAllocate(initial_size, block_length, mode));

  *old_data_file_size = initial_size;
  return Status::OK();
}

Status LBMCorruptor::GetRandomContainer(FindContainerMode mode,
                                        const Container** container) const {
  if (mode == FULL) {
    if (full_containers_.empty()) {
      return Status::IllegalState("no full containers");
    }
    *container = &full_containers_[rand_.Uniform(full_containers_.size())];
    return Status::OK();
  }

  CHECK_EQ(mode, ANY);
  if (all_containers_.empty()) {
    return Status::IllegalState("no containers");
  }
  *container = &all_containers_[rand_.Uniform(all_containers_.size())];
  return Status::OK();
}

const string& LBMCorruptor::GetRandomDataDir() const {
  return data_dirs_[rand_.Uniform(data_dirs_.size())];
}

} // namespace fs
} // namespace kudu
