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

#include "kudu/util/random_util.h"

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "kudu/gutil/port.h"
#include "kudu/gutil/walltime.h"
#include "kudu/util/env.h"
#include "kudu/util/faststring.h"
#include "kudu/util/random.h"

using std::string;

namespace kudu {

void RandomString(void* dest, size_t n, Random* rng) {
  size_t i = 0;
  uint32_t random = rng->Next();
  char* cdest = static_cast<char*>(dest);
  static const size_t sz = sizeof(random);
  if (n >= sz) {
    for (i = 0; i <= n - sz; i += sz) {
      memcpy(&cdest[i], &random, sizeof(random));
      random = rng->Next();
    }
  }
  memcpy(cdest + i, &random, n - i);
}

string RandomString(size_t n, Random* rng) {
  faststring s;
  s.resize(n);
  RandomString(s.data(), n, rng);
  return s.ToString();
}

ATTRIBUTE_NO_SANITIZE_INTEGER
uint32_t GetRandomSeed32() {
  uint32_t seed = static_cast<uint32_t>(GetCurrentTimeMicros());
  seed *= getpid();
  seed *= Env::Default()->gettid();
  return seed;
}

} // namespace kudu
