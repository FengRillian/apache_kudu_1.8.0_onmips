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

#include "kudu/consensus/opid_util.h"

#include <limits>
#include <utility>

#include <glog/logging.h>

#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/opid.pb.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/substitute.h"

namespace kudu {
namespace consensus {

const int64_t kMinimumTerm = 0;
const int64_t kMinimumOpIdIndex = 0;
const int64_t kInvalidOpIdIndex = -1;

int OpIdCompare(const OpId& left, const OpId& right) {
  DCHECK(left.IsInitialized());
  DCHECK(right.IsInitialized());
  if (PREDICT_TRUE(left.term() == right.term())) {
    return left.index() < right.index() ? -1 : left.index() == right.index() ? 0 : 1;
  }
  return left.term() < right.term() ? -1 : 1;
}

bool OpIdEquals(const OpId& left, const OpId& right) {
  DCHECK(left.IsInitialized());
  DCHECK(right.IsInitialized());
  return left.term() == right.term() && left.index() == right.index();
}

bool OpIdLessThan(const OpId& left, const OpId& right) {
  DCHECK(left.IsInitialized());
  DCHECK(right.IsInitialized());
  if (left.term() < right.term()) return true;
  if (left.term() > right.term()) return false;
  return left.index() < right.index();
}

bool OpIdBiggerThan(const OpId& left, const OpId& right) {
  DCHECK(left.IsInitialized());
  DCHECK(right.IsInitialized());
  if (left.term() > right.term()) return true;
  if (left.term() < right.term()) return false;
  return left.index() > right.index();
}

bool CopyIfOpIdLessThan(const consensus::OpId& to_compare, consensus::OpId* target) {
  if (to_compare.IsInitialized() &&
      (!target->IsInitialized() || OpIdLessThan(to_compare, *target))) {
    target->CopyFrom(to_compare);
    return true;
  }
  return false;
}

size_t OpIdHashFunctor::operator() (const OpId& id) const {
  return (id.term() + 31) ^ id.index();
}

bool OpIdEqualsFunctor::operator() (const OpId& left, const OpId& right) const {
  return OpIdEquals(left, right);
}

bool OpIdLessThanPtrFunctor::operator() (const OpId* left, const OpId* right) const {
  return OpIdLessThan(*left, *right);
}

bool OpIdIndexLessThanPtrFunctor::operator() (const OpId* left, const OpId* right) const {
  return left->index() < right->index();
}

bool OpIdCompareFunctor::operator() (const OpId& left, const OpId& right) const {
  return OpIdLessThan(left, right);
}

bool OpIdBiggerThanFunctor::operator() (const OpId& left, const OpId& right) const {
  return OpIdBiggerThan(left, right);
}

OpId MinimumOpId() {
  OpId op_id;
  op_id.set_term(0);
  op_id.set_index(0);
  return op_id;
}

OpId MaximumOpId() {
  OpId op_id;
  op_id.set_term(std::numeric_limits<int64_t>::max());
  op_id.set_index(std::numeric_limits<int64_t>::max());
  return op_id;
}

// helper hash functor for delta store ids
struct DeltaIdHashFunction {
  size_t operator()(const std::pair<int64_t, int64_t >& id) const {
    return (id.first + 31) ^ id.second;
  }
};

// helper equals functor for delta store ids
struct DeltaIdEqualsTo {
  bool operator()(const std::pair<int64_t, int64_t >& left,
                  const std::pair<int64_t, int64_t >& right) const {
    return left.first == right.first && left.second == right.second;
  }
};

std::ostream& operator<<(std::ostream& os, const consensus::OpId& op_id) {
  os << OpIdToString(op_id);
  return os;
}

std::string OpIdToString(const OpId& id) {
  if (!id.IsInitialized()) {
    return "<uninitialized op>";
  }
  return strings::Substitute("$0.$1", id.term(), id.index());
}

std::string OpsRangeString(const ConsensusRequestPB& req) {
  std::string ret;
  ret.reserve(100);
  ret.push_back('[');
  if (req.ops_size() > 0) {
    const OpId& first_op = req.ops(0).id();
    const OpId& last_op = req.ops(req.ops_size() - 1).id();
    strings::SubstituteAndAppend(&ret, "$0.$1-$2.$3",
                                 first_op.term(), first_op.index(),
                                 last_op.term(), last_op.index());
  }
  ret.push_back(']');
  return ret;
}

OpId MakeOpId(int64_t term, int64_t index) {
  CHECK_GE(term, 0);
  CHECK_GE(index, 0);
  OpId ret;
  ret.set_index(index);
  ret.set_term(term);
  return ret;
}

} // namespace consensus
}  // namespace kudu
