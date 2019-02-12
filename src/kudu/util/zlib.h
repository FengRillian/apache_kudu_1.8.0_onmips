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
#pragma once

#include <iosfwd>

#include "kudu/util/slice.h"
#include "kudu/util/status.h"

namespace kudu {
namespace zlib {

// Zlib-compress the data in 'input', appending the result to 'out'.
//
// In case of an error, some data may still be appended to 'out'.
Status Compress(Slice input, std::ostream* out);

// Uncompress the zlib-compressed data in 'compressed', appending the result
// to 'out'.
//
// In case of an error, some data may still be appended to 'out'.
Status Uncompress(Slice compressed, std::ostream* out);

} // namespace zlib
} // namespace kudu
