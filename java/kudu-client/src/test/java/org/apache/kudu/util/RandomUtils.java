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
package org.apache.kudu.util;

import org.apache.yetus.audience.InterfaceAudience;
import org.apache.yetus.audience.InterfaceStability;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.Random;

@InterfaceAudience.Private
@InterfaceStability.Unstable
public class RandomUtils {
  private static final Logger LOG = LoggerFactory.getLogger(RandomUtils.class);

  private static final String TEST_RANDOM_SEED_PROP = "testRandomSeed";

  /**
   * Get an instance of Random for use in tests and logs the seed used.
   *
   * Uses a default seed of System.currentTimeMillis() with the option to
   * override via the testRandomSeed system property.
   */
  public static Random getRandom() {
    // First check the system property.
    long seed = System.currentTimeMillis();
    if (System.getProperty(TEST_RANDOM_SEED_PROP) != null) {
      seed = Long.parseLong(System.getProperty(TEST_RANDOM_SEED_PROP));
      LOG.info("System property {} is defined. Overriding random seed.", TEST_RANDOM_SEED_PROP, seed);
    }
    LOG.info("Using random seed: {}", seed);
    return new Random(seed);
  }
}
