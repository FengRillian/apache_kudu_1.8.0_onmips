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

#include "kudu/client/resource_metrics.h"
#include "kudu/client/resource_metrics-internal.h"

#include <map>
#include <mutex>
#include <string>

#include "kudu/gutil/map-util.h"

namespace kudu {

class simple_spinlock;

namespace client {

ResourceMetrics::ResourceMetrics() :
  data_(new Data) {}

ResourceMetrics::~ResourceMetrics() {
  delete data_;
}

void ResourceMetrics::Increment(const std::string& name, int64_t amount) {
  data_->Increment(name, amount);
}

std::map<std::string, int64_t> ResourceMetrics::Get() const {
  return data_->Get();
}

int64_t ResourceMetrics::GetMetric(const std::string& name) const {
  return data_->GetMetric(name);
}

ResourceMetrics::Data::Data() {}

ResourceMetrics::Data::~Data() {}

void ResourceMetrics::Data::Increment(const std::string& name, int64_t amount) {
  std::lock_guard<simple_spinlock> l(lock_);
  counters_[name] += amount;
}

std::map<std::string, int64_t> ResourceMetrics::Data::Get() const {
  std::lock_guard<simple_spinlock> l(lock_);
  return counters_;
}

int64_t ResourceMetrics::Data::GetMetric(const std::string& name) const {
  std::lock_guard<simple_spinlock> l(lock_);
  return FindWithDefault(counters_, name, 0);
}

} // namespace client
} // namespace kudu
