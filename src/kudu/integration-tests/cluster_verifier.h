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

#include <string>

#include "kudu/gutil/macros.h"
#include "kudu/tools/ksck_checksum.h"
#include "kudu/util/monotime.h"
#include "kudu/util/status.h"

namespace kudu {

namespace cluster {
class MiniCluster;
} // namespace cluster

// Utility class for integration tests to verify that the cluster is in a good state.
class ClusterVerifier {
 public:
  explicit ClusterVerifier(cluster::MiniCluster* cluster);

  // Set the timeout for read/write/admin operations.
  void SetOperationsTimeout(const MonoDelta& timeout);

  // Set the amount of time which we'll retry trying to verify the cluster
  // state. We retry because it's possible that one of the replicas is behind
  // but in the process of catching up.
  void SetVerificationTimeout(const MonoDelta& timeout);

  // Set the number of concurrent scans to execute per tablet server.
  void SetScanConcurrency(int concurrency);

  // Verify that the cluster is in good state. Triggers a gtest assertion failure
  // on failure.
  //
  // Currently, this just uses ksck to verify that the different replicas of each tablet
  // eventually agree.
  void CheckCluster();

  // Argument for CheckRowCount(...) below.
  enum ComparisonMode {
    AT_LEAST,
    EXACTLY
  };

  // Check that the given table has the given number of rows. Depending on ComparisonMode,
  // the comparison could be exact or a lower bound.
  //
  // Returns a Corruption Status if the row count is not as expected.
  //
  // NOTE: this does not perform any retries. If it's possible that the replicas are
  // still converging, it's best to use CheckCluster() first, which will wait for
  // convergence.
  void CheckRowCount(const std::string& table_name,
                     ComparisonMode mode,
                     int expected_row_count);

  // The same as above, but retries until a timeout elapses.
  void CheckRowCountWithRetries(const std::string& table_name,
                                ComparisonMode mode,
                                int expected_row_count,
                                const MonoDelta& timeout);

  // Run the ksck utility against the cluster.
  Status RunKsck();

 private:
  // Implementation for CheckRowCount -- returns a Status instead of firing
  // gtest assertions.
  Status DoCheckRowCount(const std::string& table_name,
                         ComparisonMode mode,
                         int expected_row_count);


  tools::KsckChecksumOptions checksum_options_;

  cluster::MiniCluster* cluster_;

  MonoDelta operations_timeout_;

  DISALLOW_COPY_AND_ASSIGN(ClusterVerifier);
};

} // namespace kudu
