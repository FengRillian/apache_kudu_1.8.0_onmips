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
#include "kudu/tablet/delta_stats.h"

#include <cstdint>
#include <ostream>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "kudu/common/row_changelist.h"
#include "kudu/common/schema.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/tablet/tablet.pb.h"

using strings::Substitute;

namespace kudu {

using std::string;
using std::vector;

namespace tablet {

DeltaStats::DeltaStats()
    : delete_count_(0),
      reinsert_count_(0),
      max_timestamp_(Timestamp::kMin),
      min_timestamp_(Timestamp::kMax) {
}

void DeltaStats::IncrUpdateCount(ColumnId col_id, int64_t update_count) {
  DCHECK_GE(col_id, 0);
  update_counts_by_col_id_[col_id] += update_count;
}

void DeltaStats::IncrDeleteCount(int64_t delete_count) {
  delete_count_ += delete_count;
}

void DeltaStats::IncrReinsertCount(int64_t reinsert_count) {
  reinsert_count_ += reinsert_count;
}

Status DeltaStats::UpdateStats(const Timestamp& timestamp,
                               const RowChangeList& update) {
  // Decode the mutation incrementing the update count for each of the
  // columns we find present.
  RowChangeListDecoder decoder(update);
  RETURN_NOT_OK(decoder.Init());
  switch (decoder.get_type()) {
    case RowChangeList::kReinsert: {
      IncrReinsertCount(1);
    } // FALLTHROUGH INTENDED. REINSERTs contain column updates so we need to account
      // for those in the updated column stats.
    case RowChangeList::kUpdate: {
      vector<ColumnId> col_ids;
      RETURN_NOT_OK(decoder.GetIncludedColumnIds(&col_ids));
      for (const ColumnId& col_id : col_ids) {
        IncrUpdateCount(col_id, 1);
      }
      break;
    }
    case RowChangeList::kDelete: {
      IncrDeleteCount(1);
      break;
    }
    default: LOG(FATAL) << "Invalid mutation type: " << decoder.get_type();
  }

  if (min_timestamp_ > timestamp) {
    min_timestamp_ = timestamp;
  }
  if (max_timestamp_ < timestamp) {
    max_timestamp_ = timestamp;
  }

  return Status::OK();
}

int64_t DeltaStats::UpdateCount() const {
  int64_t ret = 0;
  for (const auto& entry : update_counts_by_col_id_) {
    ret += entry.second;
  }
  return ret;
}

string DeltaStats::ToString() const {
  return strings::Substitute(
      "ts range=[$0, $1], delete_count=[$2], reinsert_count=[$3], update_counts_by_col_id=[$4]",
      min_timestamp_.ToString(),
      max_timestamp_.ToString(),
      delete_count_,
      reinsert_count_,
      JoinKeysAndValuesIterator(update_counts_by_col_id_.begin(),
                                update_counts_by_col_id_.end(),
                                ":", ","));
}

void DeltaStats::ToPB(DeltaStatsPB* pb) const {
  pb->Clear();
  pb->set_delete_count(delete_count_);
  pb->set_reinsert_count(reinsert_count_);
  for (const auto& e : update_counts_by_col_id_) {
    DeltaStatsPB::ColumnStats* stats = pb->add_column_stats();
    stats->set_col_id(e.first);
    stats->set_update_count(e.second);
  }

  pb->set_max_timestamp(max_timestamp_.ToUint64());
  pb->set_min_timestamp(min_timestamp_.ToUint64());
}

Status DeltaStats::InitFromPB(const DeltaStatsPB& pb) {
  delete_count_ = pb.delete_count();
  reinsert_count_ = pb.reinsert_count();
  update_counts_by_col_id_.clear();
  for (const DeltaStatsPB::ColumnStats& stats : pb.column_stats()) {
    IncrUpdateCount(ColumnId(stats.col_id()), stats.update_count());
  }
  max_timestamp_.FromUint64(pb.max_timestamp());
  min_timestamp_.FromUint64(pb.min_timestamp());
  return Status::OK();
}

void DeltaStats::AddColumnIdsWithUpdates(std::set<ColumnId>* col_ids) const {
  for (const auto& e : update_counts_by_col_id_) {
    if (e.second > 0) {
      col_ids->insert(e.first);
    }
  }
}


} // namespace tablet
} // namespace kudu
