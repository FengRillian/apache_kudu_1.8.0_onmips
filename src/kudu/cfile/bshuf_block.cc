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
#include "kudu/cfile/bshuf_block.h"

#include <algorithm>
#include <limits>

#include "kudu/gutil/port.h"

namespace kudu {
namespace cfile {

void AbortWithBitShuffleError(int64_t val) {
  switch (val) {
    case -1:
      LOG(FATAL) << "Failed to allocate memory";
      break;
    case -11:
      LOG(FATAL) << "Missing SSE";
      break;
    case -12:
      LOG(FATAL) << "Missing AVX";
      break;
    case -80:
      LOG(FATAL) << "Input size not a multiple of 8";
      break;
    case -81:
      LOG(FATAL) << "block_size not multiple of 8";
      break;
    case -91:
      LOG(FATAL) << "Decompression error, wrong number of bytes processed";
      break;
    default:
      LOG(FATAL) << "Error internal to compression routine";
  }
}

// Template specialization for UINT32, which is used by dictionary encoding.
// It dynamically switches the element size to UINT16 or UINT8 depending on the values
// in the current block.
template<>
Slice BShufBlockBuilder<UINT32>::Finish(rowid_t ordinal_pos) {
  RememberFirstAndLastKey();
  uint32_t max_value = 0;
  for (int i = 0; i < count_; i++) {
    max_value = std::max(max_value, cell(i));
  }

  // Shrink the block of UINT32 to block of UINT8 or UINT16 whenever possible and
  // set the header information accordingly, so that the decoder can recover the
  // encoded data.
  Slice ret;
  if (max_value <= std::numeric_limits<uint8_t>::max()) {
    for (int i = 0; i < count_; i++) {
      uint32_t value = cell(i);
      uint8_t converted_value = static_cast<uint8_t>(value);
      memcpy(&data_[i * sizeof(converted_value)], &converted_value, sizeof(converted_value));
    }
    ret = Finish(ordinal_pos, sizeof(uint8_t));
    InlineEncodeFixed32(ret.mutable_data() + 16, sizeof(uint8_t));
  } else if (max_value <= std::numeric_limits<uint16_t>::max()) {
    for (int i = 0; i < count_; i++) {
      uint32_t value = cell(i);
      uint16_t converted_value = static_cast<uint16_t>(value);
      memcpy(&data_[i * sizeof(converted_value)], &converted_value, sizeof(converted_value));
    }
    ret = Finish(ordinal_pos, sizeof(uint16_t));
    InlineEncodeFixed32(ret.mutable_data() + 16, sizeof(uint16_t));
  } else {
    ret = Finish(ordinal_pos, sizeof(uint32_t));
    InlineEncodeFixed32(ret.mutable_data() + 16, sizeof(uint32_t));
  }
  return ret;
}

// Template specialization for UINT32.
template<>
Status BShufBlockDecoder<UINT32>::SeekAtOrAfterValue(const void* value_void, bool* exact) {
  uint32_t target = *reinterpret_cast<const uint32_t*>(value_void);
  int32_t left = 0;
  int32_t right = num_elems_;

  while (left != right) {
    uint32_t mid = (left + right) / 2;
    uint32_t mid_key;
    switch (size_of_elem_) {
      case 1: {
        mid_key = Decode<uint8_t>(&decoded_[mid * size_of_elem_]);
        break;
      }
      case 2: {
        mid_key = Decode<uint16_t>(&decoded_[mid * size_of_elem_]);
        break;
      }
      case 4: {
        mid_key = Decode<uint32_t>(&decoded_[mid * size_of_elem_]);
        break;
      }
    }
    if (mid_key == target) {
      cur_idx_ = mid;
      *exact = true;
      return Status::OK();
    } else if (mid_key > target) {
      right = mid;
    } else {
      left = mid + 1;
    }
  }

  *exact = false;
  cur_idx_ = left;
  if (cur_idx_ == num_elems_) {
    return Status::NotFound("after last key in block");
  }
  return Status::OK();
}

// Template specialization for UINT32, expand blocks of UINT8 or UINT16 to UINT32.
template<>
Status BShufBlockDecoder<UINT32>::CopyNextValuesToArray(size_t* n, uint8_t* array) {
  DCHECK(parsed_);
  if (PREDICT_FALSE(*n == 0 || cur_idx_ >= num_elems_)) {
    *n = 0;
    return Status::OK();
  }

  // First, copy it to the destination array without any "expansion".
  size_t max_fetch = std::min(*n, static_cast<size_t>(num_elems_ - cur_idx_));
  memcpy(array, &decoded_[cur_idx_ * size_of_elem_], max_fetch * size_of_elem_);

  *n = max_fetch;
  cur_idx_ += max_fetch;

  // Then, "expand" it out to the correct output size. We only need to do
  // the expansion for size = 1 or size = 2.
  if (size_of_elem_ == 1) {
    for (int i = max_fetch - 1; i >= 0; i--) {
      uint32_t value = array[i];
      memcpy(&array[i * sizeof(uint32_t)], &value, sizeof(value));
    }
  } else if (size_of_elem_ == 2) {
    for (int i = max_fetch - 1; i >= 0; i--) {
      uint32_t value = UNALIGNED_LOAD16(&array[i * sizeof(uint16_t)]);
      memcpy(&array[i * sizeof(uint32_t)], &value, sizeof(value));
    }
  }

  return Status::OK();
}


} // namespace cfile
} // namespace kudu
