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

#include <cstring>
#include <ostream>
#include <string>

#include <glog/logging.h>

#include "kudu/client/value-internal.h"
#include "kudu/client/value.h"
#include "kudu/common/common.pb.h"
#include "kudu/common/schema.h"
#include "kudu/common/types.h"
#include "kudu/gutil/mathlimits.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/decimal_util.h"
#include "kudu/util/int128.h"
#include "kudu/util/status.h"

using std::string;
using strings::Substitute;

namespace kudu {
namespace client {

KuduValue::KuduValue(Data* d)
: data_(d) {
}

KuduValue::~KuduValue() {
  if (data_->type_ == Data::SLICE) {
    delete[] data_->slice_val_.data();
  }
  delete data_;
}

KuduValue* KuduValue::Clone() const {
  switch (data_->type_) {
    case Data::INT:
      return KuduValue::FromInt(data_->int_val_);
    case Data::DOUBLE:
      return KuduValue::FromDouble(data_->double_val_);
    case Data::FLOAT:
      return KuduValue::FromFloat(data_->float_val_);
    case Data::SLICE:
      return KuduValue::CopyString(data_->slice_val_);
    case Data::DECIMAL:
      return KuduValue::FromDecimal(data_->decimal_val_, data_->scale_);
  }
  LOG(FATAL);
}

KuduValue* KuduValue::FromInt(int64_t v) {
  auto d = new Data;
  d->type_ = Data::INT;
  d->int_val_ = v;

  return new KuduValue(d);
}

KuduValue* KuduValue::FromDouble(double v) {
  auto d = new Data;
  d->type_ = Data::DOUBLE;
  d->double_val_ = v;

  return new KuduValue(d);
}


KuduValue* KuduValue::FromFloat(float v) {
  auto d = new Data;
  d->type_ = Data::FLOAT;
  d->float_val_ = v;

  return new KuduValue(d);
}

KuduValue* KuduValue::FromBool(bool v) {
  auto d = new Data;
  d->type_ = Data::INT;
  d->int_val_ = v ? 1 : 0;

  return new KuduValue(d);
}

KuduValue* KuduValue::FromDecimal(int128_t dv, int8_t scale) {
  auto d = new Data;
  d->type_ = Data::DECIMAL;
  d->decimal_val_ = dv;
  d->scale_ = scale;

  return new KuduValue(d);
}

KuduValue* KuduValue::CopyString(Slice s) {
  auto copy = new uint8_t[s.size()];
  memcpy(copy, s.data(), s.size());

  auto d = new Data;
  d->type_ = Data::SLICE;
  d->slice_val_ = Slice(copy, s.size());

  return new KuduValue(d);
}

Status KuduValue::Data::CheckTypeAndGetPointer(const string& col_name,
                                               DataType t,
                                               ColumnTypeAttributes type_attributes,
                                               void** val_void) {
  const TypeInfo* ti = GetTypeInfo(t);
  switch (t) {
    case kudu::INT8:
    case kudu::INT16:
    case kudu::INT32:
    case kudu::INT64:
    case kudu::UNIXTIME_MICROS:
      RETURN_NOT_OK(CheckAndPointToInt(col_name, ti->size(), val_void));
      break;

    case kudu::BOOL:
      RETURN_NOT_OK(CheckAndPointToBool(col_name, val_void));
      break;

    case kudu::FLOAT:
      RETURN_NOT_OK(CheckValType(col_name, KuduValue::Data::FLOAT, "float"));
      *val_void = &float_val_;
      break;

    case kudu::DOUBLE:
      RETURN_NOT_OK(CheckValType(col_name, KuduValue::Data::DOUBLE, "double"));
      *val_void = &double_val_;
      break;

    case kudu::DECIMAL32:
    case kudu::DECIMAL64:
    case kudu::DECIMAL128:
      RETURN_NOT_OK(CheckAndPointToDecimal(col_name, t, type_attributes, val_void));
      break;

    case kudu::BINARY:
    case kudu::STRING:
      RETURN_NOT_OK(CheckAndPointToString(col_name, val_void));
      break;

    default:
      return Status::InvalidArgument(Substitute("cannot determine value for column $0 (type $1)",
                                                col_name, ti->name()));
  }
  return Status::OK();
}

Status KuduValue::Data::CheckValType(const string& col_name,
                                     KuduValue::Data::Type type,
                                     const char* type_str) const {
  if (type_ != type) {
    return Status::InvalidArgument(
        Substitute("non-$0 value for $0 column $1", type_str, col_name));
  }
  return Status::OK();
}

Status KuduValue::Data::CheckAndPointToBool(const string& col_name,
                                            void** val_void) {
  RETURN_NOT_OK(CheckValType(col_name, KuduValue::Data::INT, "bool"));
  int64_t int_val = int_val_;
  if (int_val != 0 && int_val != 1) {
    return Status::InvalidArgument(
        Substitute("value $0 out of range for boolean column '$1'",
                   int_val, col_name));
  }
  *val_void = &int_val_;
  return Status::OK();
}

Status KuduValue::Data::CheckAndPointToInt(const string& col_name,
                                           size_t int_size,
                                           void** val_void) {
  RETURN_NOT_OK(CheckValType(col_name, KuduValue::Data::INT, "int"));

  int64_t int_min, int_max;
  if (int_size == 8) {
    int_min = MathLimits<int64_t>::kMin;
    int_max = MathLimits<int64_t>::kMax;
  } else {
    size_t int_bits = int_size * 8 - 1;
    int_max = (1LL << int_bits) - 1;
    int_min = -int_max - 1;
  }

  int64_t int_val = int_val_;
  if (int_val < int_min || int_val > int_max) {
    return Status::InvalidArgument(
        Substitute("value $0 out of range for $1-bit signed integer column '$2'",
                   int_val, int_size * 8, col_name));
  }

  *val_void = &int_val_;
  return Status::OK();
}

Status KuduValue::Data::CheckAndPointToDecimal(const string& col_name,
                                               DataType type,
                                               ColumnTypeAttributes type_attributes,
                                               void** val_void) {
  RETURN_NOT_OK(CheckValType(col_name, KuduValue::Data::DECIMAL, "decimal"));
  // TODO: Coerce when possible
  if (scale_ != type_attributes.scale) {
    return Status::InvalidArgument(
        Substitute("value scale $0 does not match the decimal column '$1' (expected $2)",
                   scale_, col_name, type_attributes.scale));
  }
  int128_t max_val = MaxUnscaledDecimal(type_attributes.precision);
  int128_t min_val = -max_val;
  if (decimal_val_ < min_val || decimal_val_ > max_val) {
    return Status::InvalidArgument(
        Substitute("value $0 out of range for decimal column '$1'",
                   decimal_val_, col_name));
  }
  *val_void = &decimal_val_;
  return Status::OK();
}

Status KuduValue::Data::CheckAndPointToString(const string& col_name,
                                              void** val_void) {
  RETURN_NOT_OK(CheckValType(col_name, KuduValue::Data::SLICE, "string"));
  *val_void = &slice_val_;
  return Status::OK();
}

const Slice KuduValue::Data::GetSlice() {
  switch (type_) {
    case INT: {
      return Slice(reinterpret_cast<uint8_t*>(&int_val_),
                   sizeof(int64_t));
    }
    case FLOAT:
      return Slice(reinterpret_cast<uint8_t*>(&float_val_),
                   sizeof(float));
    case DOUBLE:
      return Slice(reinterpret_cast<uint8_t*>(&double_val_),
                   sizeof(double));
    case DECIMAL:
      return Slice(reinterpret_cast<uint8_t*>(&decimal_val_),
                   sizeof(int128_t));
    case SLICE:
      return slice_val_;
  }
  LOG(FATAL) << "unreachable!";
}

} // namespace client
} // namespace kudu
