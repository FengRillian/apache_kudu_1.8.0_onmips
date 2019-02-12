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

#include "kudu/tablet/multi_column_writer.h"

#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include "kudu/cfile/cfile_util.h"
#include "kudu/cfile/cfile_writer.h"
#include "kudu/common/columnblock.h"
#include "kudu/common/rowblock.h"
#include "kudu/common/schema.h"
#include "kudu/fs/block_id.h"
#include "kudu/fs/block_manager.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/stl_util.h"

namespace kudu {
namespace tablet {

using cfile::CFileWriter;
using fs::BlockCreationTransaction;
using fs::CreateBlockOptions;
using fs::WritableBlock;
using std::unique_ptr;

MultiColumnWriter::MultiColumnWriter(FsManager* fs,
                                     const Schema* schema,
                                     std::string tablet_id)
  : fs_(fs),
    schema_(schema),
    finished_(false),
    tablet_id_(std::move(tablet_id)) {
}

MultiColumnWriter::~MultiColumnWriter() {
  STLDeleteElements(&cfile_writers_);
}

Status MultiColumnWriter::Open() {
  CHECK(cfile_writers_.empty());

  // Open columns.
  const CreateBlockOptions block_opts({ tablet_id_ });
  for (int i = 0; i < schema_->num_columns(); i++) {
    const ColumnSchema &col = schema_->column(i);

    cfile::WriterOptions opts;

    // Index all columns by ordinal position, so we can match up
    // the corresponding rows.
    opts.write_posidx = true;

    /// Set the column storage attributes.
    opts.storage_attributes = col.attributes();

    // If the schema has a single PK and this is the PK col
    if (i == 0 && schema_->num_key_columns() == 1) {
      opts.write_validx = true;
    }

    // Open file for write.
    unique_ptr<WritableBlock> block;
    RETURN_NOT_OK_PREPEND(fs_->CreateNewBlock(block_opts, &block),
                          "Unable to open output file for column " + col.ToString());
    BlockId block_id(block->id());

    // Create the CFile writer itself.
    gscoped_ptr<CFileWriter> writer(new CFileWriter(
        std::move(opts),
        col.type_info(),
        col.is_nullable(),
        std::move(block)));
    RETURN_NOT_OK_PREPEND(writer->Start(),
                          "Unable to Start() writer for column " + col.ToString());

    cfile_writers_.push_back(writer.release());
    block_ids_.push_back(block_id);
  }
  LOG(INFO) << "Opened CFile writers for " << cfile_writers_.size() << " column(s)";

  return Status::OK();
}

Status MultiColumnWriter::AppendBlock(const RowBlock& block) {
  for (int i = 0; i < schema_->num_columns(); i++) {
    ColumnBlock column = block.column_block(i);
    if (column.is_nullable()) {
      RETURN_NOT_OK(cfile_writers_[i]->AppendNullableEntries(column.null_bitmap(),
          column.data(), column.nrows()));
    } else {
      RETURN_NOT_OK(cfile_writers_[i]->AppendEntries(column.data(), column.nrows()));
    }
  }
  return Status::OK();
}

Status MultiColumnWriter::FinishAndReleaseBlocks(
    BlockCreationTransaction* transaction) {
  CHECK(!finished_);
  for (int i = 0; i < schema_->num_columns(); i++) {
    CFileWriter *writer = cfile_writers_[i];
    Status s = writer->FinishAndReleaseBlock(transaction);
    if (!s.ok()) {
      LOG(WARNING) << "Unable to Finish writer for column " <<
        schema_->column(i).ToString() << ": " << s.ToString();
      return s;
    }
  }
  finished_ = true;
  return Status::OK();
}

void MultiColumnWriter::GetFlushedBlocksByColumnId(std::map<ColumnId, BlockId>* ret) const {
  CHECK(finished_);
  ret->clear();
  for (int i = 0; i < schema_->num_columns(); i++) {
    (*ret)[schema_->column_id(i)] = block_ids_[i];
  }
}

size_t MultiColumnWriter::written_size() const {
  size_t size = 0;
  for (const CFileWriter *writer : cfile_writers_) {
    size += writer->written_size();
  }
  return size;
}

} // namespace tablet
} // namespace kudu
