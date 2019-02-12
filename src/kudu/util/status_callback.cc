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

#include "kudu/util/status_callback.h"

#include <ostream>

#include <glog/logging.h>

#include "kudu/gutil/port.h"
#include "kudu/util/status.h"

using std::string;

namespace kudu {

void DoNothingStatusCB(const Status& status) {}

void CrashIfNotOkStatusCB(const string& message, const Status& status) {
  if (PREDICT_FALSE(!status.ok())) {
    LOG(FATAL) << message << ": " << status.ToString();
  }
}

Status DoNothingStatusClosure() { return Status::OK(); }

} // end namespace kudu
