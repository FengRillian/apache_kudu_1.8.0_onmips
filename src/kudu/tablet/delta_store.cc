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

#include "kudu/tablet/delta_store.h"

#include <algorithm>
#include <cstdlib>

#include <glog/logging.h>

#include "kudu/common/row_changelist.h"
#include "kudu/common/scan_spec.h"
#include "kudu/common/schema.h"
#include "kudu/common/timestamp.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/gutil/strings/strcat.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/tablet/delta_stats.h"
#include "kudu/tablet/deltafile.h"
#include "kudu/util/memory/arena.h"

namespace kudu {
namespace tablet {

using std::shared_ptr;
using std::string;
using std::vector;
using strings::Substitute;

string DeltaKeyAndUpdate::Stringify(DeltaType type, const Schema& schema, bool pad_key) const {
  return StrCat(Substitute("($0 delta key=$2, change_list=$1)",
                           DeltaType_Name(type),
                           RowChangeList(cell).ToString(schema),
                           (pad_key ? StringPrintf("%06u@tx%06u", key.row_idx(),
                                                   atoi(key.timestamp().ToString().c_str()))
                                    : Substitute("$0@tx$1", key.row_idx(),
                                                 key.timestamp().ToString()))));
}

Status DebugDumpDeltaIterator(DeltaType type,
                              DeltaIterator* iter,
                              const Schema& schema,
                              size_t nrows,
                              vector<std::string>* out) {
  ScanSpec spec;
  spec.set_cache_blocks(false);
  RETURN_NOT_OK(iter->Init(&spec));
  RETURN_NOT_OK(iter->SeekToOrdinal(0));

  const size_t kRowsPerBlock = 100;

  Arena arena(32 * 1024);
  for (size_t i = 0; iter->HasNext(); ) {
    size_t n;
    if (nrows > 0) {
      if (i >= nrows) {
        break;
      }
      n = std::min(kRowsPerBlock, nrows - i);
    } else {
      n = kRowsPerBlock;
    }

    arena.Reset();

    RETURN_NOT_OK(iter->PrepareBatch(n, DeltaIterator::PREPARE_FOR_COLLECT));
    vector<DeltaKeyAndUpdate> cells;
    RETURN_NOT_OK(iter->FilterColumnIdsAndCollectDeltas(
                      vector<ColumnId>(),
                      &cells,
                      &arena));
    for (const DeltaKeyAndUpdate& cell : cells) {
      LOG_STRING(INFO, out) << cell.Stringify(type, schema, true /*pad_key*/ );
    }

    i += n;
  }
  return Status::OK();
}

template<DeltaType Type>
Status WriteDeltaIteratorToFile(DeltaIterator* iter,
                                size_t nrows,
                                DeltaFileWriter* out) {
  ScanSpec spec;
  spec.set_cache_blocks(false);
  RETURN_NOT_OK(iter->Init(&spec));
  RETURN_NOT_OK(iter->SeekToOrdinal(0));

  const size_t kRowsPerBlock = 100;
  DeltaStats stats;
  Arena arena(32 * 1024);
  for (size_t i = 0; iter->HasNext(); ) {
    size_t n;
    if (nrows > 0) {
      if (i >= nrows) {
        break;
      }
      n = std::min(kRowsPerBlock, nrows - i);
    } else {
      n = kRowsPerBlock;
    }

    arena.Reset();

    RETURN_NOT_OK(iter->PrepareBatch(n, DeltaIterator::PREPARE_FOR_COLLECT));
    vector<DeltaKeyAndUpdate> cells;
    RETURN_NOT_OK(iter->FilterColumnIdsAndCollectDeltas(vector<ColumnId>(),
                                                        &cells,
                                                        &arena));
    for (const DeltaKeyAndUpdate& cell : cells) {
      RowChangeList rcl(cell.cell);
      RETURN_NOT_OK(out->AppendDelta<Type>(cell.key, rcl));
      RETURN_NOT_OK(stats.UpdateStats(cell.key.timestamp(), rcl));
    }

    i += n;
  }
  out->WriteDeltaStats(stats);
  return Status::OK();
}

template
Status WriteDeltaIteratorToFile<REDO>(DeltaIterator* iter,
                                      size_t nrows,
                                      DeltaFileWriter* out);

template
Status WriteDeltaIteratorToFile<UNDO>(DeltaIterator* iter,
                                      size_t nrows,
                                      DeltaFileWriter* out);

} // namespace tablet
} // namespace kudu
