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
package org.apache.kudu.mapreduce;

import static java.nio.charset.StandardCharsets.UTF_8;
import static org.apache.kudu.test.KuduTestHarness.DEFAULT_SLEEP;
import static org.apache.kudu.util.ClientTestUtil.countRowsInScan;
import static org.apache.kudu.util.ClientTestUtil.getBasicCreateTableOptions;
import static org.apache.kudu.util.ClientTestUtil.getBasicSchema;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.io.LongWritable;
import org.apache.hadoop.io.NullWritable;
import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Job;
import org.apache.hadoop.mapreduce.Mapper;
import org.apache.hadoop.mapreduce.lib.input.FileInputFormat;
import org.apache.hadoop.mapreduce.lib.input.TextInputFormat;
import org.apache.kudu.test.KuduTestHarness;
import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;

import org.apache.kudu.client.AsyncKuduScanner;
import org.apache.kudu.client.Insert;
import org.apache.kudu.client.KuduTable;
import org.apache.kudu.client.Operation;
import org.apache.kudu.client.PartialRow;

public class ITOutputFormatJob {

  private static final String TABLE_NAME =
      ITOutputFormatJob.class.getName() + "-" + System.currentTimeMillis();

  private static final HadoopTestingUtility HADOOP_UTIL = new HadoopTestingUtility();

  @Rule
  public KuduTestHarness harness = new KuduTestHarness();

  @Before
  public void setUp() throws Exception {
    harness.getClient().createTable(TABLE_NAME, getBasicSchema(), getBasicCreateTableOptions());
  }

  @After
  public void tearDown() throws Exception {
    HADOOP_UTIL.cleanup();
  }

  @Test
  @SuppressWarnings("deprecation")
  public void test() throws Exception {
    Configuration conf = new Configuration();
    String testHome =
        HADOOP_UTIL.setupAndGetTestDir(ITOutputFormatJob.class.getName(), conf).getAbsolutePath();
    String jobName = ITOutputFormatJob.class.getName();
    Job job = new Job(conf, jobName);


    // Create a 2 lines input file
    File data = new File(testHome, "data.txt");
    writeDataFile(data);
    FileInputFormat.setInputPaths(job, data.toString());

    // Configure the job to map the file and write to kudu, without reducers
    Class<TestMapperTableOutput> mapperClass = TestMapperTableOutput.class;
    job.setJarByClass(mapperClass);
    job.setMapperClass(mapperClass);
    job.setInputFormatClass(TextInputFormat.class);
    job.setNumReduceTasks(0);
    new KuduTableMapReduceUtil.TableOutputFormatConfigurator(
        job,
        TABLE_NAME,
        harness.getMasterAddressesAsString())
        .operationTimeoutMs(DEFAULT_SLEEP)
        .addDependencies(false)
        .configure();

    assertTrue("Test job did not end properly", job.waitForCompletion(true));

    // Make sure the data's there
    KuduTable table = harness.getClient().openTable(TABLE_NAME);
    AsyncKuduScanner.AsyncKuduScannerBuilder builder =
        harness.getAsyncClient().newScannerBuilder(table);
    assertEquals(2, countRowsInScan(builder.build()));
  }

  /**
   * Simple Mapper that writes one row per line, the key is the line number and the STRING column
   * is the data from that line
   */
  static class TestMapperTableOutput extends
      Mapper<LongWritable, Text, NullWritable, Operation> {

    private KuduTable table;
    @Override
    protected void map(LongWritable key, Text value, Context context) throws IOException,
        InterruptedException {
      Insert insert = table.newInsert();
      PartialRow row = insert.getRow();
      row.addInt(0, (int) key.get());
      row.addInt(1, 1);
      row.addInt(2, 2);
      row.addString(3, value.toString());
      row.addBoolean(4, true);
      context.write(NullWritable.get(), insert);
    }

    @Override
    protected void setup(Context context) throws IOException, InterruptedException {
      super.setup(context);
      table = KuduTableMapReduceUtil.getTableFromContext(context);
    }
  }

  private void writeDataFile(File data) throws IOException {
    FileOutputStream fos = new FileOutputStream(data);
    fos.write("VALUE1\nVALUE2\n".getBytes(UTF_8));
    fos.close();
  }
}
