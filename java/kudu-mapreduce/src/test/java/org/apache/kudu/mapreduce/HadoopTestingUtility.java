/**
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.apache.kudu.mapreduce;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;

import org.apache.commons.io.FileUtils;
import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.fs.FileSystem;
import org.apache.hadoop.fs.Path;

/**
 * This class is analog to HBaseTestingUtility except that we only need it for the MR tests.
 */
public class HadoopTestingUtility {

  private static final Log LOG = LogFactory.getLog(HadoopTestingUtility.class);

  private File testDir;

  private Configuration conf = new Configuration();

  /**
   * System property key to get base test directory value
   */
  public static final String BASE_TEST_DIRECTORY_KEY =
      "test.build.data.basedirectory";

  /**
   * Default base directory for test output.
   */
  private static final String DEFAULT_BASE_TEST_DIRECTORY_PREFIX = "mr-data";

  public Configuration getConfiguration() {
    return this.conf;
  }

  /**
   * Sets up a temporary directory for the test to run in. Call cleanup() at the end of your
   * tests to remove it.
   * @param testName Will be used to build a part of the directory name for the test
   * @return Where the test is homed
   */
  public File setupAndGetTestDir(String testName, Configuration conf) {
    if (this.testDir != null) {
      return this.testDir;
    }
    Path testPath = new Path(getBaseTestDir(), testName + System.currentTimeMillis());
    this.testDir = new File(testPath.toString()).getAbsoluteFile();
    this.testDir.mkdirs();
    // Set this property so when mapreduce jobs run, they will use this as their home dir.
    System.setProperty("test.build.dir", this.testDir.toString());
    System.setProperty("hadoop.home.dir", this.testDir.toString());
    conf.set("hadoop.tmp.dir", this.testDir.toString() + "/mapred");

    LOG.info("Test configured to write to " + this.testDir);
    return this.testDir;
  }

  private Path getBaseTestDir() {
    String pathName = System.getProperty(BASE_TEST_DIRECTORY_KEY);
    // Use a temporary by default if no test directory is specified.
    if (pathName == null) {
      try {
        pathName = Files.createTempDirectory(DEFAULT_BASE_TEST_DIRECTORY_PREFIX).toString();
      } catch (IOException e) {
        throw new RuntimeException(e);
      }
    }
    return new Path(pathName);
  }

  public void cleanup() throws IOException {
    FileSystem.closeAll();
    if (this.testDir != null) {
      delete(this.testDir);
    }
  }

  private void delete(File dir) throws IOException {
    if (dir == null || !dir.exists()) {
      return;
    }
    try {
      FileUtils.deleteDirectory(dir);
    } catch (IOException ex) {
      LOG.warn("Failed to delete " + dir.getAbsolutePath());
    }
  }
}
