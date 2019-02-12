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
#ifndef KUDU_TABLET_DELTA_ITERATOR_MERGER_H
#define KUDU_TABLET_DELTA_ITERATOR_MERGER_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "kudu/common/rowid.h"
#include "kudu/gutil/port.h"
#include "kudu/tablet/delta_store.h"
#include "kudu/util/status.h"

namespace kudu {

class Arena;
class ColumnBlock;
class ScanSpec;
class SelectionVector;
struct ColumnId;

namespace tablet {

class Mutation;
struct RowIteratorOptions;

// DeltaIterator that simply combines together other DeltaIterators,
// applying deltas from each in order.
class DeltaIteratorMerger : public DeltaIterator {
 public:
  // Create a new DeltaIterator which combines the deltas from
  // all of the input delta stores.
  //
  // If only one store is input, this will automatically return an unwrapped
  // iterator for greater efficiency.
  static Status Create(
      const std::vector<std::shared_ptr<DeltaStore>> &stores,
      const RowIteratorOptions& opts,
      std::unique_ptr<DeltaIterator>* out);

  ////////////////////////////////////////////////////////////
  // Implementations of DeltaIterator
  ////////////////////////////////////////////////////////////
  virtual Status Init(ScanSpec *spec) OVERRIDE;
  virtual Status SeekToOrdinal(rowid_t idx) OVERRIDE;
  virtual Status PrepareBatch(size_t nrows, PrepareFlag flag) OVERRIDE;
  virtual Status ApplyUpdates(size_t col_to_apply, ColumnBlock *dst) OVERRIDE;
  virtual Status ApplyDeletes(SelectionVector *sel_vec) OVERRIDE;
  virtual Status CollectMutations(std::vector<Mutation *> *dst, Arena *arena) OVERRIDE;
  virtual Status FilterColumnIdsAndCollectDeltas(const std::vector<ColumnId>& col_ids,
                                                 std::vector<DeltaKeyAndUpdate>* out,
                                                 Arena* arena) OVERRIDE;
  virtual bool HasNext() OVERRIDE;
  bool MayHaveDeltas() override;
  virtual std::string ToString() const OVERRIDE;

 private:
  explicit DeltaIteratorMerger(std::vector<std::unique_ptr<DeltaIterator> > iters);

  std::vector<std::unique_ptr<DeltaIterator> > iters_;
};

} // namespace tablet
} // namespace kudu

#endif // KUDU_TABLET_DELTA_ITERATOR_MERGER_H
