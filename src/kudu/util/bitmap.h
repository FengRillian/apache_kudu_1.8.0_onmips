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
//
// Utility functions for dealing with a byte array as if it were a bitmap.
#ifndef KUDU_UTIL_BITMAP_H
#define KUDU_UTIL_BITMAP_H

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>

#include <glog/logging.h>

#include "kudu/gutil/bits.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/fastmem.h"

namespace kudu {

// Return the number of bytes necessary to store the given number of bits.
inline size_t BitmapSize(size_t num_bits) {
  return (num_bits + 7) / 8;
}

// Set the given bit.
inline void BitmapSet(uint8_t *bitmap, size_t idx) {
  bitmap[idx >> 3] |= 1 << (idx & 7);
}

// Switch the given bit to the specified value.
inline void BitmapChange(uint8_t *bitmap, size_t idx, bool value) {
  bitmap[idx >> 3] = (bitmap[idx >> 3] & ~(1 << (idx & 7))) | ((!!value) << (idx & 7));
}

// Clear the given bit.
inline void BitmapClear(uint8_t *bitmap, size_t idx) {
  bitmap[idx >> 3] &= ~(1 << (idx & 7));
}

// Test/get the given bit.
inline bool BitmapTest(const uint8_t *bitmap, size_t idx) {
  return bitmap[idx >> 3] & (1 << (idx & 7));
}

// Merge the two bitmaps using bitwise or. Both bitmaps should have at least
// n_bits valid bits.
inline void BitmapMergeOr(uint8_t *dst, const uint8_t *src, size_t n_bits) {
  size_t n_bytes = BitmapSize(n_bits);
  for (size_t i = 0; i < n_bytes; i++) {
    *dst++ |= *src++;
  }
}

// Set bits from offset to (offset + num_bits) to the specified value
void BitmapChangeBits(uint8_t *bitmap, size_t offset, size_t num_bits, bool value);

// Find the first bit of the specified value, starting from the specified offset.
bool BitmapFindFirst(const uint8_t *bitmap, size_t offset, size_t bitmap_size,
                     bool value, size_t *idx);

// Find the first set bit in the bitmap, at the specified offset.
inline bool BitmapFindFirstSet(const uint8_t *bitmap, size_t offset,
                               size_t bitmap_size, size_t *idx) {
  return BitmapFindFirst(bitmap, offset, bitmap_size, true, idx);
}

// Find the first zero bit in the bitmap, at the specified offset.
inline bool BitmapFindFirstZero(const uint8_t *bitmap, size_t offset,
                                size_t bitmap_size, size_t *idx) {
  return BitmapFindFirst(bitmap, offset, bitmap_size, false, idx);
}

// Returns true if the bitmap contains only ones.
inline bool BitMapIsAllSet(const uint8_t *bitmap, size_t offset, size_t bitmap_size) {
  DCHECK_LT(offset, bitmap_size);
  size_t idx;
  return !BitmapFindFirstZero(bitmap, offset, bitmap_size, &idx);
}

// Returns true if the bitmap contains only zeros.
inline bool BitmapIsAllZero(const uint8_t *bitmap, size_t offset, size_t bitmap_size) {
  DCHECK_LT(offset, bitmap_size);
  size_t idx;
  return !BitmapFindFirstSet(bitmap, offset, bitmap_size, &idx);
}

// Returns true if the two bitmaps are equal.
//
// It is assumed that both bitmaps have 'bitmap_size' number of bits.
inline bool BitmapEquals(const uint8_t* bm1, const uint8_t* bm2, size_t bitmap_size) {
  // Use memeq() to check all of the full bytes.
  size_t num_full_bytes = bitmap_size >> 3;
  if (!strings::memeq(bm1, bm2, num_full_bytes)) {
    return false;
  }

  // Check any remaining bits in one extra operation.
  size_t num_remaining_bits = bitmap_size - (num_full_bytes << 3);
  if (num_remaining_bits == 0) {
    return true;
  }
  DCHECK_LT(num_remaining_bits, 8);
  uint8_t mask = (1 << num_remaining_bits) - 1;
  return (bm1[num_full_bytes] & mask) == (bm2[num_full_bytes] & mask);
}

std::string BitmapToString(const uint8_t *bitmap, size_t num_bits);

// Iterator which yields ranges of set and unset bits.
// Example usage:
//   bool value;
//   size_t size;
//   BitmapIterator iter(bitmap, n_bits);
//   while ((size = iter.Next(&value))) {
//      printf("bitmap block len=%lu value=%d\n", size, value);
//   }
class BitmapIterator {
 public:
  BitmapIterator(const uint8_t *map, size_t num_bits)
    : offset_(0), num_bits_(num_bits), map_(map)
  {}

  bool done() const {
    return (num_bits_ - offset_) == 0;
  }

  void SeekTo(size_t bit) {
    DCHECK_LE(bit, num_bits_);
    offset_ = bit;
  }

  size_t Next(bool *value) {
    size_t len = num_bits_ - offset_;
    if (PREDICT_FALSE(len == 0))
      return(0);

    *value = BitmapTest(map_, offset_);

    size_t index;
    if (BitmapFindFirst(map_, offset_, num_bits_, !(*value), &index)) {
      len = index - offset_;
    } else {
      index = num_bits_;
    }

    offset_ = index;
    return len;
  }

 private:
  size_t offset_;
  size_t num_bits_;
  const uint8_t *map_;
};

// Iterator which yields the set bits in a bitmap.
// Example usage:
//   for (TrueBitIterator iter(bitmap, n_bits);
//        !iter.done();
//        ++iter) {
//     int next_onebit_position = *iter;
//   }
class TrueBitIterator {
 public:
  TrueBitIterator(const uint8_t *bitmap, size_t n_bits)
    : bitmap_(bitmap),
      cur_byte_(0),
      cur_byte_idx_(0),
      n_bits_(n_bits),
      n_bytes_(BitmapSize(n_bits_)),
      bit_idx_(0) {
    if (n_bits_ == 0) {
      cur_byte_idx_ = 1; // sets done
    } else {
      cur_byte_ = bitmap[0];
      AdvanceToNextOneBit();
    }
  }

  TrueBitIterator &operator ++() {
    DCHECK(!done());
    DCHECK(cur_byte_ & 1);
    cur_byte_ &= (~1);
    AdvanceToNextOneBit();
    return *this;
  }

  bool done() const {
    return cur_byte_idx_ >= n_bytes_;
  }

  size_t operator *() const {
    DCHECK(!done());
    return bit_idx_;
  }

 private:
  void AdvanceToNextOneBit() {
    while (cur_byte_ == 0) {
      cur_byte_idx_++;
      if (cur_byte_idx_ >= n_bytes_) return;
      cur_byte_ = bitmap_[cur_byte_idx_];
      bit_idx_ = cur_byte_idx_ * 8;
    }
    DVLOG(2) << "Found next nonzero byte at " << cur_byte_idx_
             << " val=" << cur_byte_;

    DCHECK_NE(cur_byte_, 0);
    int set_bit = Bits::FindLSBSetNonZero(cur_byte_);
    bit_idx_ += set_bit;
    cur_byte_ >>= set_bit;
  }

  const uint8_t *bitmap_;
  uint8_t cur_byte_;
  uint8_t cur_byte_idx_;

  const size_t n_bits_;
  const size_t n_bytes_;
  size_t bit_idx_;
};

} // namespace kudu

#endif
