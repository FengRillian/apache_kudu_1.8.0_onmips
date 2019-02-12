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

#include "kudu/util/int128.h"

#include <iostream>
#include <string>

#include "kudu/gutil/strings/numbers.h"

namespace std {

// Support the << operator on int128_t and uint128_t types.
//
inline std::ostream& operator<<(std::ostream& os, const __int128& val) {
  os << SimpleItoa(val);
  return os;
}
inline std::ostream& operator<<(std::ostream& os, const unsigned __int128& val) {
  os << SimpleItoa(val);
  return os;
}

} // namespace std

