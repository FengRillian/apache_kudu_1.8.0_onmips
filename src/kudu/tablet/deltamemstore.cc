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

#include "kudu/tablet/deltamemstore.h"

#include <cstring>
#include <ostream>
#include <utility>

#include <glog/logging.h>

#include "kudu/common/columnblock.h"
#include "kudu/common/row.h"
#include "kudu/common/row_changelist.h"
#include "kudu/common/rowblock.h"
#include "kudu/common/schema.h"
#include "kudu/common/timestamp.h"
#include "kudu/common/types.h"
#include "kudu/consensus/opid.pb.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/tablet/deltafile.h"
#include "kudu/tablet/mutation.h"
#include "kudu/tablet/mvcc.h"
#include "kudu/util/debug-util.h"
#include "kudu/util/faststring.h"
#include "kudu/util/memcmpable_varint.h"
#include "kudu/util/memory/memory.h"
#include "kudu/util/status.h"

namespace kudu {
namespace tablet {

using fs::IOContext;
using log::LogAnchorRegistry;
using std::string;
using std::shared_ptr;
using std::vector;
using strings::Substitute;

////////////////////////////////////////////////////////////
// DeltaMemStore implementation
////////////////////////////////////////////////////////////

static const int kInitialArenaSize = 16;

Status DeltaMemStore::Create(int64_t id,
                             int64_t rs_id,
                             LogAnchorRegistry* log_anchor_registry,
                             shared_ptr<MemTracker> parent_tracker,
                             shared_ptr<DeltaMemStore>* dms) {
  shared_ptr<DeltaMemStore> local_dms(new DeltaMemStore(id, rs_id,
                                                        log_anchor_registry,
                                                        std::move(parent_tracker)));

  dms->swap(local_dms);
  return Status::OK();
}

DeltaMemStore::DeltaMemStore(int64_t id,
                             int64_t rs_id,
                             LogAnchorRegistry* log_anchor_registry,
                             shared_ptr<MemTracker> parent_tracker)
  : id_(id),
    rs_id_(rs_id),
    allocator_(new MemoryTrackingBufferAllocator(
        HeapBufferAllocator::Get(), std::move(parent_tracker))),
    arena_(new ThreadSafeMemoryTrackingArena(kInitialArenaSize, allocator_)),
    tree_(arena_),
    anchorer_(log_anchor_registry,
              Substitute("Rowset-$0/DeltaMemStore-$1", rs_id_, id_)),
    disambiguator_sequence_number_(0) {
}

Status DeltaMemStore::Init(const IOContext* /*io_context*/) {
  return Status::OK();
}

Status DeltaMemStore::Update(Timestamp timestamp,
                             rowid_t row_idx,
                             const RowChangeList &update,
                             const consensus::OpId& op_id) {
  DeltaKey key(row_idx, timestamp);

  faststring buf;

  key.EncodeTo(&buf);

  Slice key_slice(buf);
  btree::PreparedMutation<DMSTreeTraits> mutation(key_slice);
  mutation.Prepare(&tree_);
  if (PREDICT_FALSE(mutation.exists())) {
    // We already have a delta for this row at the same timestamp.
    // Try again with a disambiguating sequence number appended to the key.
    int seq = disambiguator_sequence_number_.Increment();
    PutMemcmpableVarint64(&buf, seq);
    key_slice = Slice(buf);
    mutation.Reset(key_slice);
    mutation.Prepare(&tree_);
    CHECK(!mutation.exists())
      << "Appended a sequence number but still hit a duplicate "
      << "for rowid " << row_idx << " at timestamp " << timestamp;
  }
  if (PREDICT_FALSE(!mutation.Insert(update.slice()))) {
    return Status::IOError("Unable to insert into tree");
  }

  anchorer_.AnchorIfMinimum(op_id.index());

  return Status::OK();
}

Status DeltaMemStore::FlushToFile(DeltaFileWriter *dfw,
                                  gscoped_ptr<DeltaStats>* stats_ret) {
  gscoped_ptr<DeltaStats> stats(new DeltaStats());

  gscoped_ptr<DMSTreeIter> iter(tree_.NewIterator());
  iter->SeekToStart();
  while (iter->IsValid()) {
    Slice key_slice, val;
    iter->GetCurrentEntry(&key_slice, &val);
    DeltaKey key;
    RETURN_NOT_OK(key.DecodeFrom(&key_slice));
    RowChangeList rcl(val);
    RETURN_NOT_OK_PREPEND(dfw->AppendDelta<REDO>(key, rcl), "Failed to append delta");
    stats->UpdateStats(key.timestamp(), rcl);
    iter->Next();
  }
  dfw->WriteDeltaStats(*stats);

  stats_ret->swap(stats);
  return Status::OK();
}

Status DeltaMemStore::NewDeltaIterator(const RowIteratorOptions& opts,
                                       DeltaIterator** iterator) const {
  *iterator = new DMSIterator(shared_from_this(), opts);
  return Status::OK();
}

Status DeltaMemStore::CheckRowDeleted(rowid_t row_idx,
                                      const IOContext* /*io_context*/,
                                      bool *deleted) const {
  *deleted = false;

  DeltaKey key(row_idx, Timestamp(0));
  faststring buf;
  key.EncodeTo(&buf);
  Slice key_slice(buf);

  bool exact;

  // TODO(unknown): can we avoid the allocation here?
  gscoped_ptr<DMSTreeIter> iter(tree_.NewIterator());
  if (!iter->SeekAtOrAfter(key_slice, &exact)) {
    return Status::OK();
  }

  while (iter->IsValid()) {
    // Iterate forward until reaching an entry with a larger row idx.
    Slice key_slice, v;
    iter->GetCurrentEntry(&key_slice, &v);
    RETURN_NOT_OK(key.DecodeFrom(&key_slice));
    DCHECK_GE(key.row_idx(), row_idx);
    if (key.row_idx() != row_idx) break;

    RowChangeList val(v);
    // Mutation is for the target row, check deletion status.
    RowChangeListDecoder decoder((RowChangeList(v)));
    decoder.InitNoSafetyChecks();
    decoder.TwiddleDeleteStatus(deleted);

    iter->Next();
  }

  return Status::OK();
}

void DeltaMemStore::DebugPrint() const {
  tree_.DebugPrint();
}

////////////////////////////////////////////////////////////
// DMSIterator
////////////////////////////////////////////////////////////

DMSIterator::DMSIterator(const shared_ptr<const DeltaMemStore>& dms,
                         RowIteratorOptions opts)
    : dms_(dms),
      opts_(std::move(opts)),
      iter_(dms->tree_.NewIterator()),
      initted_(false),
      prepared_idx_(0),
      prepared_count_(0),
      prepared_for_(NOT_PREPARED),
      seeked_(false) {}

Status DMSIterator::Init(ScanSpec* /*spec*/) {
  initted_ = true;
  return Status::OK();
}

Status DMSIterator::SeekToOrdinal(rowid_t row_idx) {
  faststring buf;
  DeltaKey key(row_idx, Timestamp(0));
  key.EncodeTo(&buf);

  bool exact; /* unused */
  iter_->SeekAtOrAfter(Slice(buf), &exact);
  prepared_idx_ = row_idx;
  prepared_count_ = 0;
  prepared_for_ = NOT_PREPARED;
  seeked_ = true;
  return Status::OK();
}

Status DMSIterator::PrepareBatch(size_t nrows, PrepareFlag flag) {
  // This current implementation copies the whole batch worth of deltas
  // into a buffer local to this iterator, after filtering out deltas which
  // aren't yet committed in the current MVCC snapshot. The theory behind
  // this approach is the following:

  // Each batch needs to be processed once per column, meaning that unless
  // we make a local copy, we'd have to reset the CBTree iterator back to the
  // start of the batch and re-iterate for each column. CBTree iterators make
  // local copies as they progress in order to shield from concurrent mutation,
  // so with N columns, we'd end up making N copies of the data. Making a local
  // copy here is instead a single copy of the data, so is likely faster.
  CHECK(seeked_);
  DCHECK(initted_) << "must init";
  rowid_t start_row = prepared_idx_ + prepared_count_;
  rowid_t stop_row = start_row + nrows - 1;

  if (updates_by_col_.empty()) {
    updates_by_col_.resize(opts_.projection->num_columns());
  }
  for (UpdatesForColumn& ufc : updates_by_col_) {
    ufc.clear();
  }
  deleted_.clear();
  prepared_deltas_.clear();

  while (iter_->IsValid()) {
    Slice key_slice, val;
    iter_->GetCurrentEntry(&key_slice, &val);
    DeltaKey key;
    RETURN_NOT_OK(key.DecodeFrom(&key_slice));
    DCHECK_GE(key.row_idx(), start_row);
    if (key.row_idx() > stop_row) break;

    if (!opts_.snap_to_include.IsCommitted(key.timestamp())) {
      // The transaction which applied this update is not yet committed
      // in this iterator's MVCC snapshot. Hence, skip it.
      iter_->Next();
      continue;
    }

    if (flag == PREPARE_FOR_APPLY) {
      RowChangeListDecoder decoder((RowChangeList(val)));
      decoder.InitNoSafetyChecks();
      DCHECK(!decoder.is_reinsert()) << "Reinserts are not supported in the DeltaMemStore.";
      if (decoder.is_delete()) {
        deleted_.push_back(key.row_idx());
      } else {
        DCHECK(decoder.is_update());
        while (decoder.HasNext()) {
          RowChangeListDecoder::DecodedUpdate dec;
          RETURN_NOT_OK(decoder.DecodeNext(&dec));
          int col_idx;
          const void* col_val;
          RETURN_NOT_OK(dec.Validate(*opts_.projection, &col_idx, &col_val));
          if (col_idx == -1) {
            // This column isn't being projected.
            continue;
          }
          int col_size = opts_.projection->column(col_idx).type_info()->size();

          // If we already have an earlier update for the same column, we can
          // just overwrite that one.
          if (updates_by_col_[col_idx].empty() ||
              updates_by_col_[col_idx].back().row_id != key.row_idx()) {
            updates_by_col_[col_idx].emplace_back();
          }

          ColumnUpdate& cu = updates_by_col_[col_idx].back();
          cu.row_id = key.row_idx();
          if (col_val == nullptr) {
            cu.new_val_ptr = nullptr;
          } else {
            memcpy(cu.new_val_buf, col_val, col_size);
            // NOTE: we're constructing a pointer here to an element inside the deque.
            // This is safe because deques never invalidate pointers to their elements.
            cu.new_val_ptr = cu.new_val_buf;
          }
        }
      }
    } else {
      DCHECK_EQ(flag, PREPARE_FOR_COLLECT);
      PreparedDelta d;
      d.key = key;
      d.val = val;
      prepared_deltas_.push_back(d);
    }

    iter_->Next();
  }
  prepared_idx_ = start_row;
  prepared_count_ = nrows;
  prepared_for_ = flag == PREPARE_FOR_APPLY ? PREPARED_FOR_APPLY : PREPARED_FOR_COLLECT;
  return Status::OK();
}

Status DMSIterator::ApplyUpdates(size_t col_to_apply, ColumnBlock *dst) {
  DCHECK_EQ(prepared_for_, PREPARED_FOR_APPLY);
  DCHECK_EQ(prepared_count_, dst->nrows());

  const ColumnSchema* col_schema = &opts_.projection->column(col_to_apply);
  for (const ColumnUpdate& cu : updates_by_col_[col_to_apply]) {
    int32_t idx_in_block = cu.row_id - prepared_idx_;
    DCHECK_GE(idx_in_block, 0);
    SimpleConstCell src(col_schema, cu.new_val_ptr);
    ColumnBlock::Cell dst_cell = dst->cell(idx_in_block);
    RETURN_NOT_OK(CopyCell(src, &dst_cell, dst->arena()));
  }

  return Status::OK();
}


Status DMSIterator::ApplyDeletes(SelectionVector *sel_vec) {
  DCHECK_EQ(prepared_for_, PREPARED_FOR_APPLY);
  DCHECK_EQ(prepared_count_, sel_vec->nrows());

  for (auto& row_id : deleted_) {
    uint32_t idx_in_block = row_id - prepared_idx_;
    sel_vec->SetRowUnselected(idx_in_block);
  }

  return Status::OK();
}


Status DMSIterator::CollectMutations(vector<Mutation *> *dst, Arena *arena) {
  DCHECK_EQ(prepared_for_, PREPARED_FOR_COLLECT);
  for (const PreparedDelta& src : prepared_deltas_) {
    DeltaKey key = src.key;;
    RowChangeList changelist(src.val);
    uint32_t rel_idx = key.row_idx() - prepared_idx_;

    Mutation *mutation = Mutation::CreateInArena(arena, key.timestamp(), changelist);
    mutation->PrependToList(&dst->at(rel_idx));
  }
  return Status::OK();
}

Status DMSIterator::FilterColumnIdsAndCollectDeltas(const vector<ColumnId>& col_ids,
                                                    vector<DeltaKeyAndUpdate>* out,
                                                    Arena* arena) {
  LOG(DFATAL) << "Attempt to call FilterColumnIdsAndCollectDeltas on DMS" << GetStackTrace();
  return Status::InvalidArgument("FilterColumsAndAppend() is not supported by DMSIterator");
}

bool DMSIterator::HasNext() {
  // TODO implement this if we ever want to include DeltaMemStore in minor
  // delta compaction.
  LOG(FATAL) << "Unimplemented";
  return false;
}

bool DMSIterator::MayHaveDeltas() {
  if (!deleted_.empty()) {
    return true;
  }
  for (auto& col: updates_by_col_) {
    if (!col.empty()) {
      return true;
    }
  }
  return false;
}

string DMSIterator::ToString() const {
  return "DMSIterator";
}

} // namespace tablet
} // namespace kudu
