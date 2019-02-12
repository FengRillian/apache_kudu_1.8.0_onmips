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

#include <memory>
#include <string>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/fs/block_id.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/tablet/rowset_metadata.h"
#include "kudu/tablet/tablet_metadata.h"
#include "kudu/util/status.h"
#include "kudu/util/test_util.h"

using std::unique_ptr;
using std::vector;
using std::string;
using strings::Substitute;

namespace kudu {
namespace tablet {

class MetadataTest : public KuduTest {
 public:
  MetadataTest() {
    all_blocks_ = { BlockId(1), BlockId(2), BlockId(3), BlockId(4) };

    tablet_meta_ = new TabletMetadata(nullptr, "fake-tablet");
    CHECK_OK(RowSetMetadata::CreateNew(tablet_meta_.get(), 0, &meta_));
    for (int i = 0; i < all_blocks_.size(); i++) {
      CHECK_OK(meta_->CommitRedoDeltaDataBlock(i, all_blocks_[i]));
    }
    CHECK_EQ(4, meta_->redo_delta_blocks().size());
  }

 protected:
  vector<BlockId> all_blocks_;
  scoped_refptr<TabletMetadata> tablet_meta_;
  unique_ptr<RowSetMetadata> meta_;
};

// Swap out some deltas from the middle of the list
TEST_F(MetadataTest, RSMD_TestReplaceDeltas_1) {
  vector<BlockId> to_replace;
  to_replace.emplace_back(2);
  to_replace.emplace_back(3);

  vector<BlockId> removed;
  meta_->CommitUpdate(
      RowSetMetadataUpdate()
      .ReplaceRedoDeltaBlocks(to_replace, { BlockId(123) }), &removed);
  ASSERT_EQ(vector<BlockId>({ BlockId(1), BlockId(123), BlockId(4) }),
            meta_->redo_delta_blocks());
  ASSERT_EQ(vector<BlockId>({ BlockId(2), BlockId(3) }),
            removed);
}

// Swap out some deltas from the beginning of the list
TEST_F(MetadataTest, RSMD_TestReplaceDeltas_2) {
  vector<BlockId> to_replace;
  to_replace.emplace_back(1);
  to_replace.emplace_back(2);

  vector<BlockId> removed;
  meta_->CommitUpdate(
      RowSetMetadataUpdate()
      .ReplaceRedoDeltaBlocks(to_replace, { BlockId(123) }), &removed);
  ASSERT_EQ(vector<BlockId>({ BlockId(123), BlockId(3), BlockId(4) }),
            meta_->redo_delta_blocks());
  ASSERT_EQ(vector<BlockId>({ BlockId(1), BlockId(2) }),
            removed);
}

// Swap out some deltas from the end of the list
TEST_F(MetadataTest, RSMD_TestReplaceDeltas_3) {
  vector<BlockId> to_replace;
  to_replace.emplace_back(3);
  to_replace.emplace_back(4);

  vector<BlockId> removed;
  meta_->CommitUpdate(
      RowSetMetadataUpdate()
      .ReplaceRedoDeltaBlocks(to_replace, { BlockId(123) }), &removed);
  ASSERT_EQ(vector<BlockId>({ BlockId(1), BlockId(2), BlockId(123) }),
            meta_->redo_delta_blocks());
  ASSERT_EQ(vector<BlockId>({ BlockId(3), BlockId(4) }),
            removed);
}

// Swap out a non-contiguous list, check error.
TEST_F(MetadataTest, RSMD_TestReplaceDeltas_Bad_NonContiguous) {
  vector<BlockId> to_replace;
  to_replace.emplace_back(2);
  to_replace.emplace_back(4);

  EXPECT_DEATH({
    vector<BlockId> removed;
    meta_->CommitUpdate(
        RowSetMetadataUpdate().ReplaceRedoDeltaBlocks(to_replace, { BlockId(123) }),
        &removed);
  }, Substitute(".*Cannot find subsequence <$0> in <$1>.*",
                BlockId::JoinStrings(to_replace),
                BlockId::JoinStrings(all_blocks_)));
}

// Swap out a list which contains an invalid element, check error.
TEST_F(MetadataTest, RSMD_TestReplaceDeltas_Bad_DoesntExist) {
  vector<BlockId> to_replace;
  to_replace.emplace_back(555);

  EXPECT_DEATH({
    vector<BlockId> removed;
    meta_->CommitUpdate(
        RowSetMetadataUpdate().ReplaceRedoDeltaBlocks(to_replace, { BlockId(123) }),
        &removed);
  },Substitute(".*Cannot find subsequence <$0> in <$1>",
               BlockId::JoinStrings(to_replace),
               BlockId::JoinStrings(all_blocks_)));
}

} // namespace tablet
} // namespace kudu
