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
#include <boost/utility/binary.hpp>
#include <gtest/gtest.h>
#include "kudu/util/bit-util.h"

namespace kudu {

TEST(BitUtil, TrailingBits) {
  EXPECT_EQ(BitUtil::TrailingBits(BOOST_BINARY(1 1 1 1 1 1 1 1), 0), 0);
  EXPECT_EQ(BitUtil::TrailingBits(BOOST_BINARY(1 1 1 1 1 1 1 1), 1), 1);
  EXPECT_EQ(BitUtil::TrailingBits(BOOST_BINARY(1 1 1 1 1 1 1 1), 64),
            BOOST_BINARY(1 1 1 1 1 1 1 1));
  EXPECT_EQ(BitUtil::TrailingBits(BOOST_BINARY(1 1 1 1 1 1 1 1), 100),
            BOOST_BINARY(1 1 1 1 1 1 1 1));
  EXPECT_EQ(BitUtil::TrailingBits(0, 1), 0);
  EXPECT_EQ(BitUtil::TrailingBits(0, 64), 0);
  EXPECT_EQ(BitUtil::TrailingBits(1LL << 63, 0), 0);
  EXPECT_EQ(BitUtil::TrailingBits(1LL << 63, 63), 0);
  EXPECT_EQ(BitUtil::TrailingBits(1LL << 63, 64), 1LL << 63);

}

TEST(BitUtil, ShiftBits) {
  EXPECT_EQ(BitUtil::ShiftLeftZeroOnOverflow(1ULL, 64), 0ULL);
  EXPECT_EQ(BitUtil::ShiftLeftZeroOnOverflow(0xFFFFFFFFFFFFFFFFULL, 32), 0xFFFFFFFF00000000ULL);
  EXPECT_EQ(BitUtil::ShiftRightZeroOnOverflow(1ULL, 64), 0ULL);
  EXPECT_EQ(BitUtil::ShiftRightZeroOnOverflow(0xFFFFFFFFFFFFFFFFULL, 32), 0x00000000FFFFFFFFULL);
}

} // namespace kudu
