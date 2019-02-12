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
package org.apache.kudu.client;

import static org.apache.kudu.test.KuduTestHarness.DEFAULT_SLEEP;
import static org.apache.kudu.util.ClientTestUtil.createBasicSchemaInsert;
import static org.apache.kudu.util.ClientTestUtil.createFourTabletsTableWithNineRows;
import static org.apache.kudu.util.ClientTestUtil.getBasicCreateTableOptions;
import static org.apache.kudu.util.ClientTestUtil.getBasicSchema;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.util.List;

import org.apache.kudu.Schema;
import org.apache.kudu.test.KuduTestHarness;
import org.junit.Rule;
import org.junit.Test;

public class TestRowErrors {

  private static final Schema basicSchema = getBasicSchema();

  private static KuduTable table;

  @Rule
  public KuduTestHarness harness = new KuduTestHarness();

  @Test(timeout = 100000)
  public void singleTabletTest() throws Exception {
    String tableName = TestRowErrors.class.getName() + "-" + System.currentTimeMillis();
    harness.getClient().createTable(tableName, basicSchema, getBasicCreateTableOptions());
    table = harness.getClient().openTable(tableName);
    AsyncKuduSession session = harness.getAsyncClient().newSession();

    // Insert 3 rows to play with.
    for (int i = 0; i < 3; i++) {
      session.apply(createInsert(i)).join(DEFAULT_SLEEP);
    }

    // Try a single dupe row insert with AUTO_FLUSH_SYNC.
    Insert dupeForZero = createInsert(0);
    OperationResponse resp = session.apply(dupeForZero).join(DEFAULT_SLEEP);
    assertTrue(resp.hasRowError());
    assertTrue(resp.getRowError().getOperation() == dupeForZero);

    // Now try inserting two dupes and one good row, make sure we get only two errors back.
    dupeForZero = createInsert(0);
    Insert dupeForTwo = createInsert(2);
    session.setFlushMode(AsyncKuduSession.FlushMode.MANUAL_FLUSH);
    session.apply(dupeForZero);
    session.apply(dupeForTwo);
    session.apply(createInsert(4));

    List<OperationResponse> responses = session.flush().join(DEFAULT_SLEEP);
    List<RowError> errors = OperationResponse.collectErrors(responses);
    assertEquals(2, errors.size());
    assertTrue(errors.get(0).getOperation() == dupeForZero);
    assertTrue(errors.get(1).getOperation() == dupeForTwo);
  }

  /**
   * Test collecting errors from multiple tablets.
   * @throws Exception
   */
  @Test(timeout = 100000)
  public void multiTabletTest() throws Exception {
    String tableName = TestRowErrors.class.getName() + "-" + System.currentTimeMillis();
    createFourTabletsTableWithNineRows(harness.getAsyncClient(), tableName, DEFAULT_SLEEP);
    table = harness.getClient().openTable(tableName);
    KuduSession session = harness.getClient().newSession();
    session.setFlushMode(KuduSession.FlushMode.AUTO_FLUSH_BACKGROUND);

    int dupRows = 3;
    session.apply(createInsert(12));
    session.apply(createInsert(22));
    session.apply(createInsert(32));

    session.flush();

    RowErrorsAndOverflowStatus reos = session.getPendingErrors();
    assertEquals(dupRows, reos.getRowErrors().length);
    assertEquals(0, session.countPendingErrors());
  }

  private Insert createInsert(int key) {
    return createBasicSchemaInsert(table, key);
  }
}
