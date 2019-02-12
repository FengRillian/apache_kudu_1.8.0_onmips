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

#ifndef KUDU_CFILE_BLOCK_ENCODINGS_H
#define KUDU_CFILE_BLOCK_ENCODINGS_H

#include <algorithm>
#include <stdint.h>
#include <glog/logging.h>

#include "kudu/common/column_materialization_context.h"
#include "kudu/common/rowid.h"
#include "kudu/common/rowblock.h"
#include "kudu/cfile/cfile.pb.h"
#include "kudu/gutil/macros.h"
#include "kudu/util/faststring.h"
#include "kudu/util/slice.h"
#include "kudu/util/status.h"

namespace kudu {
class ColumnDataView;

namespace cfile {
class CFileWriter;

class BlockBuilder {
 public:
  BlockBuilder() { }

  // Append extra information to the end of the current cfile, for example:
  // append the dictionary block for under dictionary encoding mode.
  virtual Status AppendExtraInfo(CFileWriter *c_writer, CFileFooterPB* footer) {
    return Status::OK();
  }

  // Used by the cfile writer to determine whether the current block is full.
  // A block is full if it its estimated size is larger than the configured
  // WriterOptions' cfile_block_size.
  // If it is full, the cfile writer will call FinishCurDataBlock().
  virtual bool IsBlockFull() const = 0;

  // Add a sequence of values to the block.
  // Returns the number of values actually added, which may be less
  // than requested if the block is full.
  virtual int Add(const uint8_t *vals, size_t count) = 0;

  // Return a Slice which represents the encoded data.
  //
  // This Slice points to internal data of this class
  // and becomes invalid after the builder is destroyed
  // or after Finish() is called again.
  virtual Slice Finish(rowid_t ordinal_pos) = 0;

  // Reset the internal state of the encoder.
  //
  // Any data previously returned by Finish or by GetFirstKey
  // may be invalidated by this call.
  //
  // Postcondition: Count() == 0
  virtual void Reset() = 0;

  // Return the number of entries that have been added to the
  // block.
  virtual size_t Count() const = 0;

  // Return the key of the first entry in this index block.
  // For pointer-based types (such as strings), the pointed-to
  // data is only valid until the next call to Reset().
  //
  // If no keys have been added, returns Status::NotFound
  virtual Status GetFirstKey(void *key) const = 0;

  // Return the key of the last entry in this index block.
  // For pointer-based types (such as strings), the pointed-to
  // data is only valid until the next call to Reset().
  //
  // If no keys have been added, returns Status::NotFound
  virtual Status GetLastKey(void *key) const = 0;

  virtual ~BlockBuilder() {}
 private:
  DISALLOW_COPY_AND_ASSIGN(BlockBuilder);
};


class BlockDecoder {
 public:
  BlockDecoder() { }

  virtual Status ParseHeader() = 0;

  // Seek the decoder to the given positional index of the block.
  // For example, SeekToPositionInBlock(0) seeks to the first
  // stored entry.
  //
  // It is an error to call this with a value larger than Count().
  // Doing so has undefined results.
  //
  // TODO: Since we know the actual file position, maybe we
  // should just take the actual ordinal in the file
  // instead of the position in the block?
  virtual void SeekToPositionInBlock(uint pos) = 0;

  // Seek the decoder to the given value in the block, or the
  // lowest value which is greater than the given value.
  //
  // If the decoder was able to locate an exact match, then
  // sets *exact_match to true. Otherwise sets *exact_match to
  // false, to indicate that the seeked value is _after_ the
  // requested value.
  //
  // If the given value is less than the lowest value in the block,
  // seeks to the start of the block. If it is higher than the highest
  // value in the block, then returns Status::NotFound
  //
  // This will only return valid results when the data block
  // consists of values in sorted order.
  virtual Status SeekAtOrAfterValue(const void *value,
                                    bool *exact_match) = 0;

  // Seek the decoder forward by a given number of rows, or to the end
  // of the block. This is primarily used to skip over data.
  //
  // If *n would move the index past the end of the block, set *n to the
  // number of rows to get to the end.
  virtual void SeekForward(int* n) {
    DCHECK(HasNext());
    *n = std::min(*n, static_cast<int>(Count() - GetCurrentIndex()));
    DCHECK_GE(*n, 0);
    SeekToPositionInBlock(GetCurrentIndex() + *n);
  }

  // Fetch the next set of values from the block into 'dst'.
  // The output block must have space for up to n cells.
  //
  // Modifies *n to contain the number of values fetched.
  //
  // In the case that the values are themselves references
  // to other memory (eg Slices), the referred-to memory is
  // allocated in the dst block's arena.
  virtual Status CopyNextValues(size_t *n, ColumnDataView *dst) = 0;

  // Fetch the next values from the block and evaluate whether they satisfy
  // the predicate. Mark the row in the view into the selection vector. This
  // view denotes the current location in the CFile.
  //
  // Modifies *n to contain the number of values fetched.
  //
  // POSTCONDITION: ctx->decoder_eval_supported_ is not kNotSet. State must
  // be consistent throughout the entire column.
  virtual Status CopyNextAndEval(size_t* n,
                                 ColumnMaterializationContext* ctx,
                                 SelectionVectorView* sel,
                                 ColumnDataView* dst) {
    RETURN_NOT_OK(CopyNextValues(n, dst));
    ctx->SetDecoderEvalNotSupported();
    return Status::OK();
  }

  // Return true if there are more values remaining to be iterated.
  // (i.e that the next call to CopyNextValues will return at least 1
  // element)
  // TODO: change this to a Remaining() call?
  virtual bool HasNext() const = 0;

  // Return the number of elements in this block.
  virtual size_t Count() const = 0;

  // Return the position within the block of the currently seeked
  // entry (ie the entry that will next be returned by CopyNextValues())
  virtual size_t GetCurrentIndex() const = 0;

  // Return the first rowid stored in this block.
  // TODO: get rid of this from the block decoder, and put it in a generic
  // header which is shared by all data blocks.
  virtual rowid_t GetFirstRowId() const = 0;

  virtual ~BlockDecoder() {}
 private:
  DISALLOW_COPY_AND_ASSIGN(BlockDecoder);
};

} // namespace cfile
} // namespace kudu

#endif
