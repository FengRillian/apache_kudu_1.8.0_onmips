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

#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/strings/numbers.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/crc.h"
#include "kudu/util/stopwatch.h"
#include "kudu/util/test_util.h"

namespace kudu {
namespace crc {

using strings::Substitute;

class CrcTest : public KuduTest {
 protected:

  // Returns pointer to data which must be deleted by caller.
  static void GenerateBenchmarkData(const uint8_t** bufptr, size_t* buflen) {
    const uint32_t kNumNumbers = 1000000;
    const uint32_t kBytesPerNumber = sizeof(uint32_t);
    const uint32_t kLength = kNumNumbers * kBytesPerNumber;
    auto buf = new uint8_t[kLength];
    for (uint32_t i = 0; i < kNumNumbers; i++) {
      memcpy(buf + (i * kBytesPerNumber), &i, kBytesPerNumber);
    }
    *bufptr = buf;
    *buflen = kLength;
  }

};

// Basic functionality test.
TEST_F(CrcTest, TestCRC32C) {
  const std::string test_data("abcdefgh");
  const uint64_t kExpectedCrc = 0xa9421b7; // Known value from crcutil usage test program.

  Crc* crc32c = GetCrc32cInstance();
  uint64_t data_crc = 0;
  crc32c->Compute(test_data.data(), test_data.length(), &data_crc);
  char buf[kFastToBufferSize];
  const char* output = FastHex64ToBuffer(data_crc, buf);
  LOG(INFO) << "CRC32C of " << test_data << " is: 0x" << output << " (full 64 bits)";
  output = FastHex32ToBuffer(static_cast<uint32_t>(data_crc), buf);
  LOG(INFO) << "CRC32C of " << test_data << " is: 0x" << output << " (truncated 32 bits)";
  ASSERT_EQ(kExpectedCrc, data_crc);

  // Using helper
  uint64_t data_crc2 = Crc32c(test_data.data(), test_data.length());
  ASSERT_EQ(kExpectedCrc, data_crc2);

  // Using multiple chunks
  size_t half_length = test_data.length() / 2;
  uint64_t data_crc3 = Crc32c(test_data.data(), half_length);
  data_crc3 = Crc32c(test_data.data() + half_length, half_length, data_crc3);
  ASSERT_EQ(kExpectedCrc, data_crc3);
}

// Simple benchmark of CRC32C throughput.
// We should expect about 8 bytes per cycle in throughput on a single core.
TEST_F(CrcTest, BenchmarkCRC32C) {
  gscoped_ptr<const uint8_t[]> data;
  const uint8_t* buf;
  size_t buflen;
  GenerateBenchmarkData(&buf, &buflen);
  data.reset(buf);
  Crc* crc32c = GetCrc32cInstance();
  int kNumRuns = 1000;
  if (AllowSlowTests()) {
    kNumRuns = 40000;
  }
  const uint64_t kNumBytes = kNumRuns * buflen;
  Stopwatch sw;
  sw.start();
  for (int i = 0; i < kNumRuns; i++) {
    uint64_t cksum;
    crc32c->Compute(buf, buflen, &cksum);
  }
  sw.stop();
  CpuTimes elapsed = sw.elapsed();
  LOG(INFO) << Substitute("$0 runs of CRC32C on $1 bytes of data (total: $2 bytes)"
                          " in $3 seconds; $4 bytes per millisecond, $5 bytes per nanosecond!",
                          kNumRuns, buflen, kNumBytes, elapsed.wall_seconds(),
                          (kNumBytes / elapsed.wall_millis()),
                          (kNumBytes / elapsed.wall));
}

} // namespace crc
} // namespace kudu
