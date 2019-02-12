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

#include "kudu/cfile/binary_plain_block.h"

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <string>

#include <glog/logging.h>

#include "kudu/cfile/cfile_util.h"
#include "kudu/common/column_materialization_context.h"
#include "kudu/common/column_predicate.h"
#include "kudu/common/columnblock.h"
#include "kudu/common/common.pb.h"
#include "kudu/common/rowblock.h"
#include "kudu/common/schema.h"
#include "kudu/common/types.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/coding.h"
#include "kudu/util/coding-inl.h"
#include "kudu/util/group_varint-inl.h"
#include "kudu/util/hexdump.h"
#include "kudu/util/memory/arena.h"

namespace kudu {
namespace cfile {

BinaryPlainBlockBuilder::BinaryPlainBlockBuilder(const WriterOptions *options)
  : end_of_data_offset_(0),
    size_estimate_(0),
    options_(options) {
  Reset();
}

void BinaryPlainBlockBuilder::Reset() {
  offsets_.clear();
  buffer_.clear();
  buffer_.resize(kMaxHeaderSize);
  buffer_.reserve(options_->storage_attributes.cfile_block_size);

  size_estimate_ = kMaxHeaderSize;
  end_of_data_offset_ = kMaxHeaderSize;
  finished_ = false;
}

bool BinaryPlainBlockBuilder::IsBlockFull() const {
  return size_estimate_ > options_->storage_attributes.cfile_block_size;
}

Slice BinaryPlainBlockBuilder::Finish(rowid_t ordinal_pos) {
  finished_ = true;

  size_t offsets_pos = buffer_.size();

  // Set up the header
  InlineEncodeFixed32(&buffer_[0], ordinal_pos);
  InlineEncodeFixed32(&buffer_[4], offsets_.size());
  InlineEncodeFixed32(&buffer_[8], offsets_pos);

  // append the offsets, if non-empty
  if (!offsets_.empty()) {
    coding::AppendGroupVarInt32Sequence(&buffer_, 0, &offsets_[0], offsets_.size());
  }

  return Slice(buffer_);
}

int BinaryPlainBlockBuilder::Add(const uint8_t *vals, size_t count) {
  DCHECK(!finished_);
  DCHECK_GT(count, 0);
  size_t i = 0;

  // If the block is full, should stop adding more items.
  while (!IsBlockFull() && i < count) {

    // Every fourth entry needs a gvint selector byte
    // TODO(todd): does it cost a lot to account these things specifically?
    // maybe cheaper to just over-estimate - allocation is cheaper than math?
    if (offsets_.size() % 4 == 0) {
      size_estimate_++;
    }

    const Slice *src = reinterpret_cast<const Slice *>(vals);
    size_t offset = buffer_.size();
    offsets_.push_back(offset);
    size_estimate_ += coding::CalcRequiredBytes32(offset);

    buffer_.append(src->data(), src->size());
    size_estimate_ += src->size();

    i++;
    vals += sizeof(Slice);
  }

  end_of_data_offset_ = buffer_.size();

  return i;
}


size_t BinaryPlainBlockBuilder::Count() const {
  return offsets_.size();
}

Status BinaryPlainBlockBuilder::GetKeyAtIdx(void *key_void, int idx) const {
  Slice *slice = reinterpret_cast<Slice *>(key_void);

  if (idx >= offsets_.size()) {
    return Status::InvalidArgument("index too large");
  }

  if (offsets_.empty()) {
    return Status::NotFound("no keys in data block");
  }

  if (PREDICT_FALSE(offsets_.size() == 1)) {
    *slice = Slice(&buffer_[kMaxHeaderSize],
                   end_of_data_offset_ - kMaxHeaderSize);
  } else if (idx + 1 == offsets_.size()) {
    *slice = Slice(&buffer_[offsets_[idx]],
                   end_of_data_offset_ - offsets_[idx]);
  } else {
    *slice = Slice(&buffer_[offsets_[idx]],
                   offsets_[idx + 1] - offsets_[idx]);
  }
  return Status::OK();
}

Status BinaryPlainBlockBuilder::GetFirstKey(void *key_void) const {
  CHECK(finished_);
  return GetKeyAtIdx(key_void, 0);
}

Status BinaryPlainBlockBuilder::GetLastKey(void *key_void) const {
  CHECK(finished_);
  return GetKeyAtIdx(key_void, offsets_.size() - 1);
}

////////////////////////////////////////////////////////////
// Decoding
////////////////////////////////////////////////////////////

BinaryPlainBlockDecoder::BinaryPlainBlockDecoder(Slice slice)
    : data_(slice),
      parsed_(false),
      num_elems_(0),
      ordinal_pos_base_(0),
      cur_idx_(0) {
}

Status BinaryPlainBlockDecoder::ParseHeader() {
  CHECK(!parsed_);

  if (data_.size() < kMinHeaderSize) {
    return Status::Corruption(
      strings::Substitute("not enough bytes for header: string block header "
        "size ($0) less than minimum possible header length ($1)",
        data_.size(), kMinHeaderSize));
  }

  // Decode header.
  ordinal_pos_base_  = DecodeFixed32(&data_[0]);
  num_elems_         = DecodeFixed32(&data_[4]);
  size_t offsets_pos = DecodeFixed32(&data_[8]);

  // Sanity check.
  if (offsets_pos > data_.size()) {
    return Status::Corruption(
      StringPrintf("offsets_pos %ld > block size %ld in plain string block",
                   offsets_pos, data_.size()));
  }

  // Decode the string offsets themselves
  const uint8_t *p = data_.data() + offsets_pos;
  const uint8_t *limit = data_.data() + data_.size();

  // Reserve one extra element, which we'll fill in at the end
  // with an offset past the last element.
  offsets_buf_.resize(sizeof(uint32_t) * (num_elems_ + 1));
  uint32_t* dst_ptr = reinterpret_cast<uint32_t*>(offsets_buf_.data());
  size_t rem = num_elems_;
  while (rem >= 4) {
    if (PREDICT_TRUE(p + 16 < limit)) {
      p = coding::DecodeGroupVarInt32_SSE(
          p, &dst_ptr[0], &dst_ptr[1], &dst_ptr[2], &dst_ptr[3]);

      // The above function should add at most 17 (4 32-bit ints plus a selector byte) to
      // 'p'. Thus, since we checked that (p + 16 < limit) above, we are guaranteed that
      // (p <= limit) now.
      DCHECK_LE(p, limit);
    } else {
      p = coding::DecodeGroupVarInt32_SlowButSafe(
          p, &dst_ptr[0], &dst_ptr[1], &dst_ptr[2], &dst_ptr[3]);
      if (PREDICT_FALSE(p > limit)) {
        // Only need to check 'p' overrun in the slow path, because 'p' may have
        // been within 16 bytes of 'limit'.
        LOG(WARNING) << "bad block: " << HexDump(data_);
        return Status::Corruption(StringPrintf("unable to decode offsets in block"));
      }
    }
    dst_ptr += 4;
    rem -= 4;
  }

  if (rem > 0) {
    uint32_t ints[4];
    p = coding::DecodeGroupVarInt32_SlowButSafe(p, &ints[0], &ints[1], &ints[2], &ints[3]);
    if (PREDICT_FALSE(p > limit)) {
      LOG(WARNING) << "bad block: " << HexDump(data_);
      return Status::Corruption(
        StringPrintf("unable to decode offsets in block"));
    }

    for (int i = 0; i < rem; i++) {
      *dst_ptr++ = ints[i];
    }
  }

  // Add one extra entry pointing after the last item to make the indexing easier.
  *dst_ptr++ = offsets_pos;

  parsed_ = true;

  return Status::OK();
}

void BinaryPlainBlockDecoder::SeekToPositionInBlock(uint pos) {
  if (PREDICT_FALSE(num_elems_ == 0)) {
    DCHECK_EQ(0, pos);
    return;
  }

  DCHECK_LE(pos, num_elems_);
  cur_idx_ = pos;
}

Status BinaryPlainBlockDecoder::SeekAtOrAfterValue(const void *value_void, bool *exact) {
  DCHECK(value_void != nullptr);

  const Slice &target = *reinterpret_cast<const Slice *>(value_void);

  // Binary search in restart array to find the first restart point
  // with a key >= target
  int32_t left = 0;
  int32_t right = num_elems_;
  while (left != right) {
    uint32_t mid = (left + right) / 2;
    Slice mid_key(string_at_index(mid));
    int c = mid_key.compare(target);
    if (c < 0) {
      left = mid + 1;
    } else if (c > 0) {
      right = mid;
    } else {
      cur_idx_ = mid;
      *exact = true;
      return Status::OK();
    }
  }
  *exact = false;
  cur_idx_ = left;
  if (cur_idx_ == num_elems_) {
    return Status::NotFound("after last key in block");
  }

  return Status::OK();
}

template <typename CellHandler>
Status BinaryPlainBlockDecoder::HandleBatch(size_t* n, ColumnDataView* dst, CellHandler c) {
  DCHECK(parsed_);
  CHECK_EQ(dst->type_info()->physical_type(), BINARY);
  DCHECK_LE(*n, dst->nrows());
  DCHECK_EQ(dst->stride(), sizeof(Slice));
  Arena *out_arena = dst->arena();
  if (PREDICT_FALSE(*n == 0 || cur_idx_ >= num_elems_)) {
    *n = 0;
    return Status::OK();
  }
  size_t max_fetch = std::min(*n, static_cast<size_t>(num_elems_ - cur_idx_));

  Slice *out = reinterpret_cast<Slice*>(dst->data());
  for (size_t i = 0; i < max_fetch; i++, out++, cur_idx_++) {
    Slice elem(string_at_index(cur_idx_));
    c(i, elem, out, out_arena);
  }
  *n = max_fetch;
  return Status::OK();
}

Status BinaryPlainBlockDecoder::CopyNextValues(size_t* n, ColumnDataView* dst) {
  return HandleBatch(n, dst, [&](size_t i, Slice elem, Slice* out, Arena* out_arena) {
    CHECK(out_arena->RelocateSlice(elem, out));
  });
}

Status BinaryPlainBlockDecoder::CopyNextAndEval(size_t* n,
                                                ColumnMaterializationContext* ctx,
                                                SelectionVectorView* sel,
                                                ColumnDataView* dst) {
  ctx->SetDecoderEvalSupported();
  return HandleBatch(n, dst, [&](size_t i, Slice elem, Slice* out, Arena* out_arena) {
    if (!sel->TestBit(i)) {
      return;
    } else if (ctx->pred()->EvaluateCell<BINARY>(static_cast<const void*>(&elem))) {
      CHECK(out_arena->RelocateSlice(elem, out));
    } else {
      sel->ClearBit(i);
    }
  });
}


} // namespace cfile
} // namespace kudu
