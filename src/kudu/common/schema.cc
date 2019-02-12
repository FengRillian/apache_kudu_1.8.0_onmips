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

#include "kudu/common/schema.h"

#include <algorithm>
#include <unordered_set>

#include "kudu/common/row.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/strcat.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/malloc.h"
#include "kudu/util/memory/arena.h"
#include "kudu/util/status.h"

using std::string;
using std::unordered_set;
using std::vector;
using strings::Substitute;

namespace kudu {

// In a new schema, we typically would start assigning column IDs at 0. However, this
// makes it likely that in many test cases, the column IDs and the column indexes are
// equal to each other, and it's easy to accidentally pass an index where we meant to pass
// an ID, without having any issues. So, in DEBUG builds, we start assigning columns at ID
// 10, ensuring that if we accidentally mix up IDs and indexes, we're likely to fire an
// assertion or bad memory access.
#ifdef NDEBUG
static const ColumnId kFirstColumnId(0);
#else
static const ColumnId  kFirstColumnId(10);
#endif

bool ColumnTypeAttributes::EqualsForType(ColumnTypeAttributes other,
                                         DataType type) const {
  switch (type) {
    case DECIMAL32:
    case DECIMAL64:
    case DECIMAL128:
      return precision == other.precision && scale == other.scale;
    default:
      return true; // true because unhandled types don't use ColumnTypeAttributes.
  }
}

string ColumnTypeAttributes::ToStringForType(DataType type) const {
  switch (type) {
    case DECIMAL32:
    case DECIMAL64:
    case DECIMAL128:
      return Substitute("($0, $1)", precision, scale);
    default:
      return "";
  }
}

string ColumnStorageAttributes::ToString() const {
  return Substitute("encoding=$0, compression=$1, cfile_block_size=$2",
                    EncodingType_Name(encoding),
                    CompressionType_Name(compression),
                    cfile_block_size);
}

Status ColumnSchema::ApplyDelta(const ColumnSchemaDelta& col_delta) {
  // This method does all validation up-front before making any changes to
  // the schema, so that if we return an error then we are guaranteed to
  // have had no effect
  if (type_info()->physical_type() != BINARY) {
    if (col_delta.default_value && col_delta.default_value->size() < type_info()->size()) {
      return Status::InvalidArgument("wrong size for default value");
    }
  }

  if (col_delta.new_name) {
    name_ = *col_delta.new_name;
  }

  if (col_delta.default_value) {
    const void* value = type_info()->physical_type() == BINARY ?
                        reinterpret_cast<const void *>(&col_delta.default_value.get()) :
                        reinterpret_cast<const void *>(col_delta.default_value->data());
    write_default_ = std::make_shared<Variant>(type_info()->type(), value);
  }

  if (col_delta.remove_default) {
    write_default_ = nullptr;
  }

  if (col_delta.encoding) {
    attributes_.encoding = *col_delta.encoding;
  }
  if (col_delta.compression) {
    attributes_.compression = *col_delta.compression;
  }
  if (col_delta.cfile_block_size) {
    attributes_.cfile_block_size = *col_delta.cfile_block_size;
  }
  return Status::OK();
}

// TODO(wdb): include attributes_.ToString() -- need to fix unit tests
// first
string ColumnSchema::ToString() const {
  return Substitute("$0[$1]",
                    name_,
                    TypeToString());
}

string ColumnSchema::TypeToString() const {
  return Substitute("$0$1 $2",
                    type_info_->name(),
                    type_attributes().ToStringForType(type_info()->type()),
                    is_nullable_ ? "NULLABLE" : "NOT NULL");
}

size_t ColumnSchema::memory_footprint_excluding_this() const {
  // Rough approximation.
  return name_.capacity();
}

size_t ColumnSchema::memory_footprint_including_this() const {
  return kudu_malloc_usable_size(this) + memory_footprint_excluding_this();
}

Schema::Schema(const Schema& other)
  : name_to_index_bytes_(0),
    name_to_index_(/*bucket_count=*/10,
                   NameToIndexMap::hasher(),
                   NameToIndexMap::key_equal(),
                   NameToIndexMapAllocator(&name_to_index_bytes_)) {
  CopyFrom(other);
}

Schema& Schema::operator=(const Schema& other) {
  if (&other != this) {
    CopyFrom(other);
  }
  return *this;
}

void Schema::CopyFrom(const Schema& other) {
  num_key_columns_ = other.num_key_columns_;
  cols_ = other.cols_;
  col_ids_ = other.col_ids_;
  col_offsets_ = other.col_offsets_;
  id_to_index_ = other.id_to_index_;

  // We can't simply copy name_to_index_ since the StringPiece keys
  // reference the other Schema's ColumnSchema objects.
  name_to_index_.clear();
  int i = 0;
  for (const ColumnSchema &col : cols_) {
    // The map uses the 'name' string from within the ColumnSchema object.
    name_to_index_[col.name()] = i++;
  }

  has_nullables_ = other.has_nullables_;
}

Schema::Schema(Schema&& other) noexcept
    : cols_(std::move(other.cols_)),
      num_key_columns_(other.num_key_columns_),
      col_ids_(std::move(other.col_ids_)),
      col_offsets_(std::move(other.col_offsets_)),
      name_to_index_bytes_(0),
      name_to_index_(/*bucket_count=*/10,
                     NameToIndexMap::hasher(),
                     NameToIndexMap::key_equal(),
                     NameToIndexMapAllocator(&name_to_index_bytes_)),
      id_to_index_(std::move(other.id_to_index_)),
      has_nullables_(other.has_nullables_) {
  // 'name_to_index_' uses a customer allocator which holds a pointer to
  // 'name_to_index_bytes_'. swap() will swap the contents but not the
  // allocators; std::move will move the allocator[1], which will mean the moved
  // to value will be holding a pointer to the moved-from's member, which is
  // wrong and will likely lead to use-after-free. The 'name_to_index_bytes_'
  // values should also be swapped so both schemas end up in a valid state.
  //
  // [1] STLCountingAllocator::propagate_on_container_move_assignment::value
  //     is std::true_type, which it inherits from the default Allocator.
  std::swap(name_to_index_bytes_, other.name_to_index_bytes_);
  name_to_index_.swap(other.name_to_index_);
}

Schema& Schema::operator=(Schema&& other) noexcept {
  if (&other != this) {
    cols_ = std::move(other.cols_);
    num_key_columns_ = other.num_key_columns_;
    col_ids_ = std::move(other.col_ids_);
    col_offsets_ = std::move(other.col_offsets_);
    id_to_index_ = std::move(other.id_to_index_);
    has_nullables_ = other.has_nullables_;

    // See the comment in the move constructor implementation for why we swap.
    std::swap(name_to_index_bytes_, other.name_to_index_bytes_);
    name_to_index_.swap(other.name_to_index_);
  }
  return *this;
}

Status Schema::Reset(const vector<ColumnSchema>& cols,
                     const vector<ColumnId>& ids,
                     int key_columns) {
  cols_ = cols;
  num_key_columns_ = key_columns;

  if (PREDICT_FALSE(key_columns > cols_.size())) {
    return Status::InvalidArgument(
      "Bad schema", "More key columns than columns");
  }

  if (PREDICT_FALSE(key_columns < 0)) {
    return Status::InvalidArgument(
      "Bad schema", "Cannot specify a negative number of key columns");
  }

  if (PREDICT_FALSE(!ids.empty() && ids.size() != cols_.size())) {
    return Status::InvalidArgument("Bad schema",
      "The number of ids does not match with the number of columns");
  }

  // Verify that the key columns are not nullable
  for (int i = 0; i < key_columns; ++i) {
    if (PREDICT_FALSE(cols_[i].is_nullable())) {
      return Status::InvalidArgument(
        "Bad schema", Substitute("Nullable key columns are not supported: $0",
                                 cols_[i].name()));
    }
  }

  // Calculate the offset of each column in the row format.
  col_offsets_.reserve(cols_.size() + 1);  // Include space for total byte size at the end.
  size_t off = 0;
  size_t i = 0;
  name_to_index_.clear();
  for (const ColumnSchema &col : cols_) {
    // The map uses the 'name' string from within the ColumnSchema object.
    if (!InsertIfNotPresent(&name_to_index_, col.name(), i++)) {
      return Status::InvalidArgument("Duplicate column name", col.name());
    }

    col_offsets_.push_back(off);
    off += col.type_info()->size();
  }

  // Add an extra element on the end for the total
  // byte size
  col_offsets_.push_back(off);

  // Initialize IDs mapping
  col_ids_ = ids;
  id_to_index_.clear();
  max_col_id_ = 0;
  for (int i = 0; i < ids.size(); ++i) {
    if (ids[i] > max_col_id_) {
      max_col_id_ = ids[i];
    }
    id_to_index_.set(ids[i], i);
  }

  // Determine whether any column is nullable
  has_nullables_ = false;
  for (const ColumnSchema& col : cols_) {
    if (col.is_nullable()) {
      has_nullables_ = true;
      break;
    }
  }

  return Status::OK();
}

Status Schema::CreateProjectionByNames(const std::vector<StringPiece>& col_names,
                                       Schema* out) const {
  vector<ColumnId> ids;
  vector<ColumnSchema> cols;
  for (const StringPiece& name : col_names) {
    int idx = find_column(name);
    if (idx == -1) {
      return Status::NotFound("column not found", name);
    }
    if (has_column_ids()) {
      ids.push_back(column_id(idx));
    }
    cols.push_back(column(idx));
  }
  return out->Reset(cols, ids, 0);
}

Status Schema::CreateProjectionByIdsIgnoreMissing(const std::vector<ColumnId>& col_ids,
                                                  Schema* out) const {
  vector<ColumnSchema> cols;
  vector<ColumnId> filtered_col_ids;
  for (ColumnId id : col_ids) {
    int idx = find_column_by_id(id);
    if (idx == -1) {
      continue;
    }
    cols.push_back(column(idx));
    filtered_col_ids.push_back(id);
  }
  return out->Reset(cols, filtered_col_ids, 0);
}

Schema Schema::CopyWithColumnIds() const {
  CHECK(!has_column_ids());
  vector<ColumnId> ids;
  for (int32_t i = 0; i < num_columns(); i++) {
    ids.emplace_back(kFirstColumnId + i);
  }
  return Schema(cols_, ids, num_key_columns_);
}

Schema Schema::CopyWithoutColumnIds() const {
  CHECK(has_column_ids());
  return Schema(cols_, num_key_columns_);
}

Status Schema::VerifyProjectionCompatibility(const Schema& projection) const {
  DCHECK(has_column_ids()) "The server schema must have IDs";

  if (projection.has_column_ids()) {
    return Status::InvalidArgument("User requests should not have Column IDs");
  }

  vector<string> missing_columns;
  for (const ColumnSchema& pcol : projection.columns()) {
    int index = find_column(pcol.name());
    if (index < 0) {
      missing_columns.push_back(pcol.name());
    } else if (!pcol.EqualsType(cols_[index])) {
      // TODO: We don't support query with type adaptors yet
      return Status::InvalidArgument("The column '" + pcol.name() + "' must have type " +
                                     cols_[index].TypeToString() + " found " + pcol.TypeToString());
    }
  }

  if (!missing_columns.empty()) {
    return Status::InvalidArgument("Some columns are not present in the current schema",
                                   JoinStrings(missing_columns, ", "));
  }
  return Status::OK();
}


Status Schema::GetMappedReadProjection(const Schema& projection,
                                       Schema *mapped_projection) const {
  // - The user projection may have different columns from the ones on the tablet
  // - User columns non present in the tablet are considered errors
  // - The user projection is not supposed to have the defaults or the nullable
  //   information on each field. The current tablet schema is supposed to.
  // - Each CFileSet may have a different schema and each CFileSet::Iterator
  //   must use projection from the CFileSet schema to the mapped user schema.
  RETURN_NOT_OK(VerifyProjectionCompatibility(projection));

  // Get the Projection Mapping
  vector<ColumnSchema> mapped_cols;
  vector<ColumnId> mapped_ids;

  mapped_cols.reserve(projection.num_columns());
  mapped_ids.reserve(projection.num_columns());

  for (const ColumnSchema& col : projection.columns()) {
    int index = find_column(col.name());
    DCHECK_GE(index, 0) << col.name();
    mapped_cols.push_back(cols_[index]);
    mapped_ids.push_back(col_ids_[index]);
  }

  CHECK_OK(mapped_projection->Reset(mapped_cols, mapped_ids, projection.num_key_columns()));
  return Status::OK();
}

string Schema::ToString() const {
  if (cols_.empty()) return "Schema []";

  vector<string> pk_strs;
  for (int i = 0; i < num_key_columns_; i++) {
    pk_strs.push_back(cols_[i].name());
  }

  vector<string> col_strs;
  if (has_column_ids()) {
    for (int i = 0; i < cols_.size(); ++i) {
      col_strs.push_back(Substitute("$0:$1", col_ids_[i], cols_[i].ToString()));
    }
  } else {
    for (const ColumnSchema &col : cols_) {
      col_strs.push_back(col.ToString());
    }
  }

  return StrCat("Schema [\n\tprimary key (",
                JoinStrings(pk_strs, ", "),
                "),\n\t",
                JoinStrings(col_strs, ",\n\t"),
                "\n]");
}

Status Schema::DecodeRowKey(Slice encoded_key,
                            uint8_t* buffer,
                            Arena* arena) const {
  ContiguousRow row(this, buffer);

  for (size_t col_idx = 0; col_idx < num_key_columns(); ++col_idx) {
    const ColumnSchema& col = column(col_idx);
    const KeyEncoder<faststring>& key_encoder = GetKeyEncoder<faststring>(col.type_info());
    bool is_last = col_idx == (num_key_columns() - 1);
    RETURN_NOT_OK_PREPEND(key_encoder.Decode(&encoded_key,
                                             is_last,
                                             arena,
                                             row.mutable_cell_ptr(col_idx)),
                          Substitute("Error decoding composite key component '$0'",
                                     col.name()));
  }
  return Status::OK();
}

string Schema::DebugEncodedRowKey(Slice encoded_key, StartOrEnd start_or_end) const {
  if (encoded_key.empty()) {
    switch (start_or_end) {
      case START_KEY: return "<start of table>";
      case END_KEY:   return "<end of table>";
    }
  }

  Arena arena(256);
  uint8_t* buf = reinterpret_cast<uint8_t*>(arena.AllocateBytes(key_byte_size()));
  Status s = DecodeRowKey(encoded_key, buf, &arena);
  if (!s.ok()) {
    return "<invalid key: " + s.ToString() + ">";
  }
  ConstContiguousRow row(this, buf);
  return DebugRowKey(row);
}

size_t Schema::memory_footprint_excluding_this() const {
  size_t size = 0;
  for (const ColumnSchema& col : cols_) {
    size += col.memory_footprint_excluding_this();
  }

  if (cols_.capacity() > 0) {
    size += kudu_malloc_usable_size(cols_.data());
  }
  if (col_ids_.capacity() > 0) {
    size += kudu_malloc_usable_size(col_ids_.data());
  }
  if (col_offsets_.capacity() > 0) {
    size += kudu_malloc_usable_size(col_offsets_.data());
  }
  size += name_to_index_bytes_;
  size += id_to_index_.memory_footprint_excluding_this();

  return size;
}

size_t Schema::memory_footprint_including_this() const {
  return kudu_malloc_usable_size(this) + memory_footprint_excluding_this();
}

// ============================================================================
//  Schema Builder
// ============================================================================
void SchemaBuilder::Reset() {
  cols_.clear();
  col_ids_.clear();
  col_names_.clear();
  num_key_columns_ = 0;
  next_id_ = kFirstColumnId;
}

void SchemaBuilder::Reset(const Schema& schema) {
  cols_ = schema.cols_;
  col_ids_ = schema.col_ids_;
  num_key_columns_ = schema.num_key_columns_;
  for (const auto& column : cols_) {
    col_names_.insert(column.name());
  }

  if (col_ids_.empty()) {
    for (int32_t i = 0; i < cols_.size(); ++i) {
      col_ids_.emplace_back(kFirstColumnId + i);
    }
  }
  if (col_ids_.empty()) {
    next_id_ = kFirstColumnId;
  } else {
    next_id_ = *std::max_element(col_ids_.begin(), col_ids_.end()) + 1;
  }
}

Status SchemaBuilder::AddKeyColumn(const string& name, DataType type) {
  return AddColumn(ColumnSchema(name, type), true);
}

Status SchemaBuilder::AddColumn(const string& name,
                                DataType type,
                                bool is_nullable,
                                const void *read_default,
                                const void *write_default) {
  return AddColumn(ColumnSchema(name, type, is_nullable, read_default, write_default), false);
}

Status SchemaBuilder::RemoveColumn(const string& name) {
  unordered_set<string>::const_iterator it_names = col_names_.find(name);
  if (it_names == col_names_.end()) {
    return Status::NotFound("The specified column does not exist", name);
  }

  col_names_.erase(it_names);
  for (int i = 0; i < cols_.size(); ++i) {
    if (name == cols_[i].name()) {
      cols_.erase(cols_.begin() + i);
      col_ids_.erase(col_ids_.begin() + i);
      if (i < num_key_columns_) {
        num_key_columns_--;
      }
      return Status::OK();
    }
  }

  LOG(FATAL) << "Should not reach here";
  return Status::Corruption("Unable to remove existing column");
}

Status SchemaBuilder::RenameColumn(const string& old_name, const string& new_name) {
  // check if 'new_name' is already in use
  if (col_names_.find(new_name) != col_names_.end()) {
    return Status::AlreadyPresent("The column already exists", new_name);
  }

  // check if the 'old_name' column exists
  unordered_set<string>::const_iterator it_names = col_names_.find(old_name);
  if (it_names == col_names_.end()) {
    return Status::NotFound("The specified column does not exist", old_name);
  }

  col_names_.erase(it_names);   // TODO(wdb): Should this one stay and marked as alias?
  col_names_.insert(new_name);

  for (ColumnSchema& col_schema : cols_) {
    if (old_name == col_schema.name()) {
      col_schema.set_name(new_name);
      return Status::OK();
    }
  }

  LOG(FATAL) << "Should not reach here";
  return Status::IllegalState("Unable to rename existing column");
}

Status SchemaBuilder::AddColumn(const ColumnSchema& column, bool is_key) {
  if (ContainsKey(col_names_, column.name())) {
    return Status::AlreadyPresent("The column already exists", column.name());
  }

  col_names_.insert(column.name());
  if (is_key) {
    cols_.insert(cols_.begin() + num_key_columns_, column);
    col_ids_.insert(col_ids_.begin() + num_key_columns_, next_id_);
    num_key_columns_++;
  } else {
    cols_.push_back(column);
    col_ids_.push_back(next_id_);
  }

  next_id_ = ColumnId(next_id_ + 1);
  return Status::OK();
}

Status SchemaBuilder::ApplyColumnSchemaDelta(const ColumnSchemaDelta& col_delta) {
  // if the column will be renamed, check if 'new_name' is already in use
  if (col_delta.new_name && ContainsKey(col_names_, *col_delta.new_name)) {
    return Status::AlreadyPresent("The column already exists", *col_delta.new_name);
  }

  // check if the column exists
  unordered_set<string>::const_iterator it_names = col_names_.find(col_delta.name);
  if (it_names == col_names_.end()) {
    return Status::NotFound("The specified column does not exist", col_delta.name);
  }

  for (ColumnSchema& col_schema : cols_) {
    if (col_delta.name == col_schema.name()) {
      RETURN_NOT_OK(col_schema.ApplyDelta(col_delta));
      if (col_delta.new_name) {
        // TODO(wdb): Should the old one stay, marked as an alias?
        col_names_.erase(it_names);
        col_names_.insert(*col_delta.new_name);
      }
      return Status::OK();
    }
  }

  LOG(FATAL) << "Should not reach here";
}

} // namespace kudu
