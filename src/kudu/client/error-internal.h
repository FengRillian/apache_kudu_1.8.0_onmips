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
#ifndef KUDU_CLIENT_ERROR_INTERNAL_H
#define KUDU_CLIENT_ERROR_INTERNAL_H

#include <memory>

#include "kudu/client/client.h"
#include "kudu/client/write_op.h"
#include "kudu/gutil/macros.h"
#include "kudu/util/status.h"

namespace kudu {

namespace client {

class KuduError::Data {
 public:
  Data(std::unique_ptr<KuduWriteOperation> failed_op, Status error);
  ~Data() = default;

  std::unique_ptr<KuduWriteOperation> failed_op_;
  Status status_;

  DISALLOW_COPY_AND_ASSIGN(Data);
};

} // namespace client
} // namespace kudu

#endif
