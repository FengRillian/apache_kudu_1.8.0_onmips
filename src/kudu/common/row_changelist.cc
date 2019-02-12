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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>

#include "kudu/common/common.pb.h"
#include "kudu/common/columnblock.h"
#include "kudu/common/row.h"
#include "kudu/common/rowblock.h"
#include "kudu/common/row_changelist.h"
#include "kudu/common/schema.h"
#include "kudu/common/types.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/coding.h"
#include "kudu/util/coding-inl.h"
#include "kudu/util/faststring.h"
#include "kudu/util/logging.h"
#include "kudu/util/memory/arena.h"

using std::string;
using strings::Substitute;
using strings::SubstituteAndAppend;

namespace kudu {

string RowChangeList::ToString(const Schema &schema) const {
  DCHECK_GT(encoded_data_.size(), 0);
  RowChangeListDecoder decoder(*this);

  Status s = decoder.Init();
  if (!s.ok()) {
    return "[invalid: " + s.ToString() + "]";
  }

  if (decoder.is_delete()) {
    return string("DELETE");
  }

  string ret;
  if (decoder.is_reinsert()) {
    ret = "REINSERT ";
  } else {
    CHECK(decoder.is_update()) << "Unknown changelist type!";
    ret = "SET ";
  }

  bool first = true;
  while (decoder.HasNext()) {
    if (!first) {
      ret.append(", ");
    }
    first = false;

    RowChangeListDecoder::DecodedUpdate dec;
    int col_idx;
    const void* value;
    s = decoder.DecodeNext(&dec);
    if (s.ok()) {
      s = dec.Validate(schema, &col_idx, &value);
    }

    if (!s.ok()) {
      return "[invalid update: " + s.ToString() + ", before corruption: " + ret + "]";
    }

    if (col_idx == Schema::kColumnNotFound) {
      // Unknown column.
      SubstituteAndAppend(&ret, "[unknown column id $0]=", dec.col_id);
      if (dec.null) {
        ret.append("NULL");
      } else {
        ret.append(KUDU_REDACT(dec.raw_value.ToDebugString()));
      }
    } else {
      // Known column.
      const ColumnSchema& col_schema = schema.column(col_idx);
      ret.append(col_schema.name());
      ret.append("=");
      if (value == nullptr) {
        ret.append("NULL");
      } else {
        ret.append(col_schema.Stringify(value));
      }
    }
  }

  return ret;
}

void RowChangeListEncoder::AddColumnUpdate(const ColumnSchema& col_schema,
                                           int col_id,
                                           const void* cell_ptr) {
  SetToUpdate();
  EncodeColumnMutation(col_schema, col_id, cell_ptr);
}

void RowChangeListEncoder::EncodeColumnMutation(const ColumnSchema& col_schema,
                                                int col_id,
                                                const void* cell_ptr) {
  DCHECK_NE(RowChangeList::kUninitialized, type_);
  DCHECK(type_ == RowChangeList::kUpdate || type_ == RowChangeList::kReinsert);

  Slice val_slice;
  if (cell_ptr != nullptr) {
    if (col_schema.type_info()->physical_type() == BINARY) {
      memcpy(&val_slice, cell_ptr, sizeof(val_slice));
    } else {
      val_slice = Slice(reinterpret_cast<const uint8_t*>(cell_ptr),
                        col_schema.type_info()->size());
    }
  } else {
    // NULL value.
    DCHECK(col_schema.is_nullable());
  }

  EncodeColumnMutationRaw(col_id, cell_ptr == nullptr, val_slice);
}


void RowChangeListEncoder::EncodeColumnMutationRaw(int col_id, bool is_null, Slice new_val) {
  // The type must have been set beforehand.
  DCHECK_NE(RowChangeList::kUninitialized, type_);
  DCHECK(type_ == RowChangeList::kUpdate || type_ == RowChangeList::kReinsert);

  InlinePutVarint32(dst_, col_id);
  if (is_null) {
    dst_->push_back(0);
  } else {
    InlinePutVarint32(dst_, new_val.size() + 1);
    dst_->append(new_val.data(), new_val.size());
  }
}

Status RowChangeListDecoder::Init() {
  if (PREDICT_FALSE(remaining_.empty())) {
    return Status::Corruption("empty changelist - expected type");
  }

  bool was_valid = tight_enum_test_cast<RowChangeList::ChangeType>(remaining_[0], &type_);
  if (PREDICT_FALSE(!was_valid || type_ == RowChangeList::kUninitialized)) {
    return Status::Corruption(Substitute("bad type enum value: $0 in $1",
                                         static_cast<int>(remaining_[0]),
                                         KUDU_REDACT(remaining_.ToDebugString())));
  }
  if (PREDICT_FALSE(is_delete() && remaining_.size() != 1)) {
    return Status::Corruption("DELETE changelist too long",
                              KUDU_REDACT(remaining_.ToDebugString()));
  }

  remaining_.remove_prefix(1);

  // We should discard empty UPDATE RowChangeLists, so if after getting
  // the type remaining_ is empty() return an error.
  // Note that REINSERTs might have empty changelists when reinserting a row on a tablet that
  // has only primary key columns.
  if (is_update() && remaining_.empty()) {
    return Status::Corruption("empty changelist - expected column updates");
  }
  return Status::OK();
}

Status RowChangeListDecoder::ProjectChangeList(const DeltaProjector& projector,
                                               const RowChangeList& src,
                                               RowChangeListEncoder* out) {
  RowChangeListDecoder decoder(src);
  RETURN_NOT_OK(decoder.Init());
  CHECK_EQ(DCHECK_NOTNULL(out)->type_, RowChangeList::kUninitialized);

  if (decoder.is_delete()) {
    out->SetToDelete();
    return Status::OK();
  }

  DCHECK(decoder.is_reinsert() || decoder.is_update());

  while (decoder.HasNext()) {
    DecodedUpdate dec;
    RETURN_NOT_OK(decoder.DecodeNext(&dec));
    int col_idx;
    const void* new_val;
    RETURN_NOT_OK(dec.Validate(*projector.projection(), &col_idx, &new_val));
    // If the new schema doesn't have this column, throw away the update.
    if (col_idx == Schema::kColumnNotFound) {
      continue;
    }

    out->SetType(decoder.type_);
    out->EncodeColumnMutationRaw(dec.col_id, dec.null, dec.raw_value);
  }

  return Status::OK();
}

Status RowChangeListDecoder::MutateRowAndCaptureChanges(RowBlockRow* dst_row,
                                                        Arena* arena,
                                                        RowChangeListEncoder* out) {

  DCHECK(is_reinsert() || is_update());
  DCHECK(out->is_initialized());

  const Schema* dst_schema = dst_row->schema();

  while (HasNext()) {
    DecodedUpdate dec;
    RETURN_NOT_OK(DecodeNext(&dec));
    int col_idx;
    const void* value;
    RETURN_NOT_OK(dec.Validate(*dst_schema, &col_idx, &value));
    // Reinserts don't update keys so they shouldn't include the key columns.
    DCHECK(!dst_schema->is_key_column(col_idx));

    // If the delta is for a column ID not part of the projection
    // we're scanning, just skip over it.
    if (col_idx == Schema::kColumnNotFound) {
        continue;
    }

    const ColumnSchema& col_schema = dst_schema->column(col_idx);
    SimpleConstCell src(&col_schema, value);
    RowBlockRow::Cell dst_cell = dst_row->cell(col_idx);

    // save the old cell in 'out'
    out->EncodeColumnMutation(col_schema, dec.col_id, dst_cell.ptr());

    // copy the new cell to the row
    RETURN_NOT_OK(CopyCell(src, &dst_cell, arena));
  }
  return Status::OK();
}



Status RowChangeListDecoder::ApplyToOneColumn(size_t row_idx, ColumnBlock* dst_col,
                                              const Schema& dst_schema,
                                              int col_idx, Arena *arena) {
  DCHECK(is_reinsert() || is_update());

  const ColumnSchema& col_schema = dst_schema.column(col_idx);
  ColumnId col_id = dst_schema.column_id(col_idx);

  while (HasNext()) {
    DecodedUpdate dec;
    RETURN_NOT_OK(DecodeNext(&dec));
    if (dec.col_id != col_id) {
      continue;
    }

    int junk_col_idx;
    const void* new_val;
    RETURN_NOT_OK(dec.Validate(dst_schema, &junk_col_idx, &new_val));
    DCHECK_EQ(junk_col_idx, col_idx);

    SimpleConstCell src(&col_schema, new_val);
    ColumnBlock::Cell dst_cell = dst_col->cell(row_idx);
    RETURN_NOT_OK(CopyCell(src, &dst_cell, arena));
    // TODO: could potentially break; here if we're guaranteed to only have one update
    // per column in a RowChangeList (which would make sense!)
  }
  return Status::OK();
}

Status RowChangeListDecoder::RemoveColumnIdsFromChangeList(const RowChangeList& src,
                                                           const std::vector<ColumnId>& col_ids,
                                                           RowChangeListEncoder* out) {
  RowChangeListDecoder decoder(src);
  RETURN_NOT_OK(decoder.Init());

  if (decoder.is_delete()) {
    out->SetToDelete();
    return Status::OK();
  }

  DCHECK(decoder.is_reinsert() || decoder.is_update());

  while (decoder.HasNext()) {
    DecodedUpdate dec;
    RETURN_NOT_OK(decoder.DecodeNext(&dec));
    if (!std::binary_search(col_ids.begin(), col_ids.end(), dec.col_id)) {
      out->SetType(decoder.type_);
      out->EncodeColumnMutationRaw(dec.col_id, dec.null, dec.raw_value);
    }
  }

  return Status::OK();
}

Status RowChangeListDecoder::DecodeNext(DecodedUpdate* dec) {
  DCHECK_NE(type_, RowChangeList::kUninitialized) << "Must call Init()";
  // Decode the column id.
  uint32_t id;
  if (PREDICT_FALSE(!GetVarint32(&remaining_, &id))) {
    return Status::Corruption("Invalid column ID varint in delta");
  }
  dec->col_id = id;

  uint32_t size;
  if (PREDICT_FALSE(!GetVarint32(&remaining_, &size))) {
    return Status::Corruption("Invalid size varint in delta");
  }

  dec->null = size == 0;
  if (dec->null) {
    return Status::OK();
  }

  size--;

  if (PREDICT_FALSE(remaining_.size() < size)) {
    return Status::Corruption(
        Substitute("truncated value for column id $0, expected $1 bytes, only $2 remaining",
                   id, size, remaining_.size()));
  }

  dec->raw_value = Slice(remaining_.data(), size);
  remaining_.remove_prefix(size);
  return Status::OK();
}

Status RowChangeListDecoder::DecodedUpdate::Validate(const Schema& schema,
                                                     int* col_idx,
                                                     const void** value) const {
  *col_idx = schema.find_column_by_id(this->col_id);
  if (*col_idx == Schema::kColumnNotFound) {
    return Status::OK();
  }

  // It's a valid column - validate it.
  const ColumnSchema& col = schema.column(*col_idx);

  if (null) {
    if (!col.is_nullable()) {
      return Status::Corruption("decoded set-to-NULL for non-nullable column",
                                col.ToString());
    }
    *value = nullptr;
    return Status::OK();
  }

  if (col.type_info()->physical_type() == BINARY) {
    *value = &this->raw_value;
    return Status::OK();
  }

  if (PREDICT_FALSE(col.type_info()->size() != this->raw_value.size())) {
    return Status::Corruption(Substitute(
                                  "invalid value $0 for column $1",
                                  KUDU_REDACT(this->raw_value.ToDebugString()), col.ToString()));
  }

  *value = reinterpret_cast<const void*>(this->raw_value.data());

  return Status::OK();
}

} // namespace kudu
