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
import static org.apache.kudu.util.ClientTestUtil.countRowsInScan;
import static org.apache.kudu.util.ClientTestUtil.createBasicSchemaInsert;
import static org.apache.kudu.util.ClientTestUtil.defaultErrorCB;
import static org.apache.kudu.util.ClientTestUtil.getBasicCreateTableOptions;
import static org.apache.kudu.util.ClientTestUtil.getBasicSchema;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

import com.stumbleupon.async.Callback;
import com.stumbleupon.async.Deferred;
import org.apache.kudu.test.KuduTestHarness;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;

import org.apache.kudu.Schema;
import org.apache.kudu.WireProtocol.AppStatusPB;
import org.apache.kudu.tserver.Tserver.TabletServerErrorPB;

/**
 * This class can either start its own cluster or rely on an existing one.
 * By default it assumes that the master is at localhost:64000.
 * The cluster's configuration flags is found at flagsPath as defined in the pom file.
 * Set startCluster to true in order have the test start the cluster for you.
 * All those properties are set via surefire's systemPropertyVariables, meaning this:
 * $ mvn test -DstartCluster=false
 * will use an existing cluster at default address found above.
 *
 * The test creates a table with a unique(ish) name which it deletes at the end.
 */
public class TestAsyncKuduSession {
  // Generate a unique table name
  private static final String TABLE_NAME =
      TestAsyncKuduSession.class.getName()+"-"+System.currentTimeMillis();

  private static final Schema schema = getBasicSchema();

  private static AsyncKuduClient client;
  private static KuduTable table;

  @Rule
  public KuduTestHarness harness = new KuduTestHarness();

  @Before
  public void setUp() throws Exception {
    client = harness.getAsyncClient();
    table = harness.getClient().createTable(TABLE_NAME, schema, getBasicCreateTableOptions());
  }

  @Test(timeout = 100000)
  public void testBackgroundErrors() throws Exception {
    try {
      AsyncKuduSession session = client.newSession();
      session.setFlushMode(SessionConfiguration.FlushMode.AUTO_FLUSH_BACKGROUND);
      session.setFlushInterval(10);
      Batch.injectTabletServerErrorAndLatency(makeTabletServerError(), 0);

      OperationResponse resp = session.apply(createInsert(1)).join(DEFAULT_SLEEP);
      assertTrue(resp.hasRowError());
      assertTrue(
          resp.getRowError().getErrorStatus()
              .getMessage().contains(getTabletServerErrorMessage()));
      assertEquals(1, session.countPendingErrors());
    } finally {
      Batch.injectTabletServerErrorAndLatency(null, 0);
    }
  }

  /**
   * Regression test for case where an error in the previous batch could cause the next
   * batch to hang in flush()
   */
  @Test(timeout = 100000)
  public void testBatchErrorCauseSessionStuck() throws Exception {
    try {
      AsyncKuduSession session = client.newSession();
      session.setFlushMode(AsyncKuduSession.FlushMode.AUTO_FLUSH_BACKGROUND);
      session.setFlushInterval(100);
      Batch.injectTabletServerErrorAndLatency(makeTabletServerError(), 200);
      // 0ms: insert first row, which will be the first batch.
      Deferred<OperationResponse> resp1 = session.apply(createInsert(1));
      Thread.sleep(120);
      // 100ms: start to send first batch.
      // 100ms+: first batch got response from ts,
      //         will wait 200s and throw error.
      // 120ms: insert another row, which will be the second batch.
      Deferred<OperationResponse> resp2 = session.apply(createInsert(2));
      // 220ms: start to send the second batch, but first batch is inflight,
      //        so add callback to retry after first batch finishes.
      // 300ms: first batch's callback handles error, retry second batch.
      {
        OperationResponse resp = resp1.join(DEFAULT_SLEEP);
        assertTrue(resp.hasRowError());
        assertTrue(
            resp.getRowError().getErrorStatus()
                .getMessage().contains(getTabletServerErrorMessage()));
      }
      {
        OperationResponse resp = resp2.join(DEFAULT_SLEEP);
        assertTrue(resp.hasRowError());
        assertTrue(
            resp.getRowError().getErrorStatus()
                .getMessage().contains(getTabletServerErrorMessage()));
      }
      assertFalse(session.hasPendingOperations());
    } finally {
      Batch.injectTabletServerErrorAndLatency(null, 0);
    }
  }

  /**
   * Regression test for case when tablet lookup error causes original RPC to get stuck.
   * @throws Exception
   */
  @Test(timeout = 100000)
  public void testGetTableLocationsErrorCauseSessionStuck() throws Exception {
    AsyncKuduSession session = client.newSession();
    // Make sure tablet locations is cached.
    Insert insert = createInsert(1);
    session.apply(insert).join(DEFAULT_SLEEP);
    RemoteTablet rt =
        client.getTableLocationEntry(table.getTableId(), insert.partitionKey()).getTablet();
    String tabletId = rt.getTabletId();
    RpcProxy proxy = client.newRpcProxy(rt.getLeaderServerInfo());
    try {
      // Delete table so we get table not found error.
      client.deleteTable(TABLE_NAME).join();
      // Wait until tablet is deleted on TS.
      while (true) {
        ListTabletsRequest req = new ListTabletsRequest();
        Deferred<ListTabletsResponse> d = req.getDeferred();
        proxy.sendRpc(req);
        ListTabletsResponse resp = d.join();
        if (!resp.getTabletsList().contains(tabletId)) {
          break;
        }
        Thread.sleep(100);
      }

      OperationResponse response = session.apply(createInsert(1)).join(DEFAULT_SLEEP);
      assertTrue(response.hasRowError());
      assertTrue(response.getRowError().getErrorStatus().isNotFound());
    } finally {
      table = harness.getClient().createTable(TABLE_NAME, schema, getBasicCreateTableOptions());
    }
  }

  /** Regression test for a failure to correctly handle a timeout when flushing a batch. */
  @Test
  public void testInsertIntoUnavailableTablet() throws Exception {
    harness.killAllTabletServers();
    try {
      AsyncKuduSession session = client.newSession();
      session.setTimeoutMillis(1);
      OperationResponse response = session.apply(createInsert(1)).join();
      assertTrue(response.hasRowError());
      assertTrue(response.getRowError().getErrorStatus().isTimedOut());

      session.setFlushMode(SessionConfiguration.FlushMode.MANUAL_FLUSH);
      Insert insert = createInsert(1);
      session.apply(insert);
      List<OperationResponse> responses = session.flush().join();
      assertEquals(1, responses.size());
      assertTrue(responses.get(0).getRowError().getErrorStatus().isTimedOut());
    } finally {
      harness.startAllTabletServers();
    }
  }

  /**
   * Regression test for a bug in which, when a tablet client is disconnected
   * and we reconnect, we were previously leaking the old RpcProxy
   * object in the client2tablets map.
   */
  @Test(timeout = 100000)
  public void testRestartBetweenWrites() throws Exception {
    // Create a non-replicated table for this test, so that
    // we're sure when we reconnect to the leader after restarting
    // the tablet servers, it's definitely the same leader we wrote
    // to before.
    KuduTable nonReplicatedTable = harness.getClient().createTable(
        "non-replicated",
        schema,
        getBasicCreateTableOptions().setNumReplicas(1));

    try {
      // Write before doing any restarts to establish a connection.
      AsyncKuduSession session = client.newSession();
      session.setTimeoutMillis(30000);
      session.setFlushMode(SessionConfiguration.FlushMode.AUTO_FLUSH_SYNC);
      session.apply(createBasicSchemaInsert(nonReplicatedTable, 1)).join();

      int numClientsBefore = client.getConnectionListCopy().size();

      // Restart all the tablet servers.
      harness.killAllTabletServers();
      harness.startAllTabletServers();

      // Perform another write, which will require reconnecting to the same
      // tablet server that we wrote to above.
      session.apply(createBasicSchemaInsert(nonReplicatedTable, 2)).join();

      // We should not have leaked an entry in the client2tablets map.
      int numClientsAfter = client.getConnectionListCopy().size();
      assertEquals(numClientsBefore, numClientsAfter);
    } finally {
      harness.startAllTabletServers();

      client.deleteTable("non-replicated").join();
    }
  }

  @Test(timeout = 100000)
  public void test() throws Exception {

    AsyncKuduSession session = client.newSession();
    // disable the low watermark until we need it
    session.setMutationBufferLowWatermark(1f);

    // First testing KUDU-232, the cache is empty and we want to force flush. We force the flush
    // interval to be higher than the sleep time so that we don't background flush while waiting.
    // If our subsequent manual flush throws, it means the logic to block on in-flight tablet
    // lookups in flush isn't working properly.
    session.setFlushMode(AsyncKuduSession.FlushMode.AUTO_FLUSH_BACKGROUND);
    session.setFlushInterval(DEFAULT_SLEEP + 1000);
    Deferred<OperationResponse> d = session.apply(createInsert(0));
    session.flush().join(DEFAULT_SLEEP);
    assertTrue(exists(0));
    // set back to default
    session.setFlushInterval(1000);

    session.setFlushMode(AsyncKuduSession.FlushMode.AUTO_FLUSH_SYNC);
    for (int i = 1; i < 10; i++) {
      session.apply(createInsert(i)).join(DEFAULT_SLEEP);
    }

    assertEquals(10, countInRange(0, 10));

    session.setFlushMode(AsyncKuduSession.FlushMode.MANUAL_FLUSH);
    session.setMutationBufferSpace(10);

    session.apply(createInsert(10));

    try {
      session.setFlushMode(AsyncKuduSession.FlushMode.AUTO_FLUSH_SYNC);
      fail();
    } catch (IllegalArgumentException ex) {
      /* expected, flush mode remains manual */
    }

    assertFalse(exists(10));

    for (int i = 11; i < 20; i++) {
      session.apply(createInsert(i));
    }

    assertEquals(0, countInRange(10, 20));
    try {
      session.apply(createInsert(20));
      fail();
    } catch (KuduException ex) {
      /* expected, buffer would be too big */
    }
    assertEquals(0, countInRange(10, 20)); // the buffer should still be full

    session.flush().join(DEFAULT_SLEEP);
    assertEquals(10, countInRange(10, 20)); // now everything should be there

    session.flush().join(DEFAULT_SLEEP); // flushing empty buffer should be a no-op.

    session.setFlushMode(AsyncKuduSession.FlushMode.AUTO_FLUSH_BACKGROUND);

    d = session.apply(createInsert(20));
    Thread.sleep(50); // waiting a minimal amount of time to make sure the interval is in effect
    assertFalse(exists(20));
    // Add 10 items, the last one will stay in the buffer
    for (int i = 21; i < 30; i++) {
      d = session.apply(createInsert(i));
    }
    Deferred<OperationResponse> buffered = session.apply(createInsert(30));
    long now = System.currentTimeMillis();
    d.join(DEFAULT_SLEEP); // Ok to use the last d, everything is going to the buffer
    // auto flush will force flush if the buffer is full as it should be now
    // so we check that we didn't wait the full interval
    long elapsed = System.currentTimeMillis() - now;
    assertTrue(elapsed < 950);
    assertEquals(10, countInRange(20, 31));
    buffered.join();
    assertEquals(11, countInRange(20, 31));

    session.setFlushMode(AsyncKuduSession.FlushMode.AUTO_FLUSH_SYNC);
    Update update = createUpdate(30);
    PartialRow row = update.getRow();
    row.addInt(2, 999);
    row.addString(3, "updated data");
    d = session.apply(update);
    d.addErrback(defaultErrorCB);
    d.join(DEFAULT_SLEEP);
    assertEquals(31, countInRange(0, 31));

    Delete del = createDelete(30);
    d = session.apply(del);
    d.addErrback(defaultErrorCB);
    d.join(DEFAULT_SLEEP);
    assertEquals(30, countInRange(0, 31));

    session.setFlushMode(AsyncKuduSession.FlushMode.MANUAL_FLUSH);
    session.setMutationBufferSpace(35);
    for (int i = 0; i < 20; i++) {
      buffered = session.apply(createDelete(i));
    }
    assertEquals(30, countInRange(0, 31));
    session.flush();
    buffered.join(DEFAULT_SLEEP);
    assertEquals(10, countInRange(0, 31));

    for (int i = 30; i < 40; i++) {
      session.apply(createInsert(i));
    }

    for (int i = 20; i < 30; i++) {
      buffered = session.apply(createDelete(i));
    }

    assertEquals(10, countInRange(0, 40));
    session.flush();
    buffered.join(DEFAULT_SLEEP);
    assertEquals(10, countInRange(0, 40));

    // Test nulls
    // add 10 rows with the nullable column set to null
    session.setFlushMode(AsyncKuduSession.FlushMode.AUTO_FLUSH_SYNC);
    for (int i = 40; i < 50; i++) {
      session.apply(createInsertWithNull(i)).join(DEFAULT_SLEEP);
    }

    // now scan those rows and make sure the column is null
    assertEquals(10, countNullColumns(40, 50));

    // Test sending edits too fast
    session.setFlushMode(AsyncKuduSession.FlushMode.AUTO_FLUSH_BACKGROUND);
    session.setMutationBufferSpace(10);

    // The buffer has a capacity of 10, we insert 21 rows, meaning we fill the first one,
    // force flush, fill a second one before the first one could come back,
    // and the 21st row will be sent back.
    boolean gotException = false;
    for (int i = 50; i < 71; i++) {
      try {
        session.apply(createInsert(i));
      } catch (PleaseThrottleException ex) {
        gotException = true;
        assertEquals(70, i);
        // Wait for the buffer to clear
        ex.getDeferred().join(DEFAULT_SLEEP);
        session.apply(ex.getFailedRpc());
        session.flush().join(DEFAULT_SLEEP);
      }
    }
    assertTrue("Expected PleaseThrottleException", gotException);
    assertEquals(21, countInRange(50, 71));

    // Now test a more subtle issue, basically the race where we call flush from the client when
    // there's a batch already in flight. We need to finish joining only when all the data is
    // flushed.
    for (int i = 71; i < 91; i++) {
      session.apply(createInsert(i));
    }
    session.flush().join(DEFAULT_SLEEP);
    // If we only waited after the in flight batch, there would be 10 rows here.
    assertEquals(20, countInRange(71, 91));

    // Test empty scanner projection
    AsyncKuduScanner scanner = getScanner(71, 91, Collections.<String>emptyList());
    assertEquals(20, countRowsInScan(scanner));

    // Test removing the connection and then do a rapid set of inserts
    client.getConnectionListCopy().get(0).disconnect()
        .awaitUninterruptibly(DEFAULT_SLEEP);
    session.setMutationBufferSpace(1);
    for (int i = 91; i < 101; i++) {
      try {
        session.apply(createInsert(i));
      } catch (PleaseThrottleException ex) {
        // Wait for the buffer to clear
        ex.getDeferred().join(DEFAULT_SLEEP);
        session.apply(ex.getFailedRpc());
      }
    }
    session.flush().join(DEFAULT_SLEEP);
    assertEquals(10, countInRange(91, 101));

    // Test a tablet going missing or encountering a new tablet while inserting a lot
    // of data. This code used to fail in many different ways.
    client.emptyTabletsCacheForTable(table.getTableId());
    for (int i = 101; i < 151; i++) {
      Insert insert = createInsert(i);
      while (true) {
        try {
          session.apply(insert);
          break;
        } catch (PleaseThrottleException ex) {
          // Wait for the buffer to clear
          ex.getDeferred().join(DEFAULT_SLEEP);
        }
      }
    }
    session.flush().join(DEFAULT_SLEEP);
    assertEquals(50, countInRange(101, 151));

    // Test the low watermark.
    // Before the fix for KUDU-804, a change to the buffer space did not result in a change to the
    // low watermark causing this test to fail.
    session.setMutationBufferLowWatermark(0.1f);
    session.setMutationBufferSpace(10);
    session.setRandomSeed(12345); // Will make us hit the exception after 6 tries
    gotException = false;
    for (int i = 151; i < 171; i++) {
      try {
        session.apply(createInsert(i));
      } catch (PleaseThrottleException ex) {
        // We're going to hit the exception after filling up the buffer a first time then trying
        // to insert 6 more rows.
        assertEquals(167, i);
        gotException = true;
        assertTrue(ex.getMessage().contains("watermark"));
        // Once we hit the exception we wait on the batch to finish flushing and then insert the
        // rest of the data.
        ex.getDeferred().join(DEFAULT_SLEEP);
        session.apply(ex.getFailedRpc());
      }
    }
    session.flush().join(DEFAULT_SLEEP);
    assertEquals(20, countInRange(151, 171));
    assertTrue("Expected PleaseThrottleException, but it was never thrown", gotException);
  }

  private Insert createInsert(int key) {
    return createBasicSchemaInsert(table, key);
  }

  private Insert createInsertWithNull(int key) {
    Insert insert = table.newInsert();
    PartialRow row = insert.getRow();
    row.addInt(0, key);
    row.addInt(1, 2);
    row.addInt(2, 3);
    row.setNull(3);
    row.addBoolean(4, false);
    return insert;
  }

  private Update createUpdate(int key) {
    Update update = table.newUpdate();
    PartialRow row = update.getRow();
    row.addInt(0, key);
    return update;
  }

  private Delete createDelete(int key) {
    Delete delete = table.newDelete();
    PartialRow row = delete.getRow();
    row.addInt(0, key);
    return delete;
  }

  public boolean exists(final int key) throws Exception {

    AsyncKuduScanner scanner = getScanner(key, key + 1);
    final AtomicBoolean exists = new AtomicBoolean(false);

    Callback<Object, RowResultIterator> cb =
        new Callback<Object, RowResultIterator>() {
      @Override
      public Object call(RowResultIterator arg) throws Exception {
        if (arg == null) return null;
        for (RowResult row : arg) {
          if (row.getInt(0) == key) {
            exists.set(true);
            break;
          }
        }
        return null;
      }
    };

    while (scanner.hasMoreRows()) {
      Deferred<RowResultIterator> data = scanner.nextRows();
      data.addCallbacks(cb, defaultErrorCB);
      data.join(DEFAULT_SLEEP);
      if (exists.get()) {
        break;
      }
    }

    Deferred<RowResultIterator> closer = scanner.close();
    closer.join(DEFAULT_SLEEP);
    return exists.get();
  }

  public int countNullColumns(final int startKey, final int endKey) throws Exception {

    AsyncKuduScanner scanner = getScanner(startKey, endKey);
    final AtomicInteger ai = new AtomicInteger();

    Callback<Object, RowResultIterator> cb = new Callback<Object, RowResultIterator>() {
      @Override
      public Object call(RowResultIterator arg) throws Exception {
        if (arg == null) return null;
        for (RowResult row : arg) {
          if (row.isNull(3)) {
            ai.incrementAndGet();
          }
        }
        return null;
      }
    };

    while (scanner.hasMoreRows()) {
      Deferred<RowResultIterator> data = scanner.nextRows();
      data.addCallbacks(cb, defaultErrorCB);
      data.join(DEFAULT_SLEEP);
    }

    Deferred<RowResultIterator> closer = scanner.close();
    closer.join(DEFAULT_SLEEP);
    return ai.get();
  }

  public int countInRange(final int start, final int exclusiveEnd) throws Exception {

    AsyncKuduScanner scanner = getScanner(start, exclusiveEnd);
    return countRowsInScan(scanner);
  }

  private AsyncKuduScanner getScanner(int start, int exclusiveEnd) {
    return getScanner(start, exclusiveEnd, null);
  }

  private AsyncKuduScanner getScanner(int start, int exclusiveEnd,
                                             List<String> columnNames) {

    PartialRow lowerBound = schema.newPartialRow();
    lowerBound.addInt(schema.getColumnByIndex(0).getName(), start);

    PartialRow upperBound = schema.newPartialRow();
    upperBound.addInt(schema.getColumnByIndex(0).getName(), exclusiveEnd);

    return client.newScannerBuilder(table)
        .lowerBound(lowerBound)
        .exclusiveUpperBound(upperBound)
        .setProjectedColumnNames(columnNames)
        .build();
  }

  private TabletServerErrorPB makeTabletServerError() {
    return TabletServerErrorPB.newBuilder()
        .setCode(TabletServerErrorPB.Code.UNKNOWN_ERROR)
        .setStatus(AppStatusPB.newBuilder()
            .setCode(AppStatusPB.ErrorCode.UNKNOWN_ERROR)
            .setMessage(getTabletServerErrorMessage())
            .build())
        .build();
  }

  private String getTabletServerErrorMessage() {
    return "injected error for test";
  }
}
