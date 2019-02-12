/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.apache.kudu.spark.kudu

import scala.collection.JavaConverters._
import scala.collection.immutable.IndexedSeq
import scala.util.control.NonFatal
import org.apache.spark.sql.DataFrame
import org.apache.spark.sql.SQLContext
import org.apache.spark.sql.functions._
import org.apache.spark.sql.types.DataTypes
import org.apache.spark.sql.types.StructField
import org.apache.spark.sql.types.StructType
import org.junit.Assert._
import org.scalatest.Matchers
import org.apache.kudu.ColumnSchema.ColumnSchemaBuilder
import org.apache.kudu.client.CreateTableOptions
import org.apache.kudu.Schema
import org.apache.kudu.Type
import org.apache.spark.sql.execution.datasources.LogicalRelation
import org.junit.Before
import org.junit.Test

class DefaultSourceTest extends KuduTestSuite with Matchers {

  val rowCount = 10
  var sqlContext: SQLContext = _
  var rows: IndexedSeq[(Int, Int, String, Long)] = _
  var kuduOptions: Map[String, String] = _

  @Before
  def setUp(): Unit = {
    rows = insertRows(table, rowCount)

    sqlContext = ss.sqlContext

    kuduOptions =
      Map("kudu.table" -> tableName, "kudu.master" -> harness.getMasterAddressesAsString)

    sqlContext.read.options(kuduOptions).kudu.createOrReplaceTempView(tableName)
  }

  @Test
  def testTableCreation() {
    val tableName = "testcreatetable"
    if (kuduContext.tableExists(tableName)) {
      kuduContext.deleteTable(tableName)
    }
    val df = sqlContext.read.options(kuduOptions).kudu
    kuduContext.createTable(
      tableName,
      df.schema,
      Seq("key"),
      new CreateTableOptions()
        .setRangePartitionColumns(List("key").asJava)
        .setNumReplicas(1))
    kuduContext.insertRows(df, tableName)

    // now use new options to refer to the new table name
    val newOptions: Map[String, String] =
      Map("kudu.table" -> tableName, "kudu.master" -> harness.getMasterAddressesAsString)
    val checkDf = sqlContext.read.options(newOptions).kudu

    assert(checkDf.schema === df.schema)
    assertTrue(kuduContext.tableExists(tableName))
    assert(checkDf.count == 10)

    kuduContext.deleteTable(tableName)
    assertFalse(kuduContext.tableExists(tableName))
  }

  @Test
  def testTableCreationWithPartitioning() {
    val tableName = "testcreatepartitionedtable"
    if (kuduContext.tableExists(tableName)) {
      kuduContext.deleteTable(tableName)
    }
    val df = sqlContext.read.options(kuduOptions).kudu

    val kuduSchema = kuduContext.createSchema(df.schema, Seq("key"))
    val lower = kuduSchema.newPartialRow()
    lower.addInt("key", 0)
    val upper = kuduSchema.newPartialRow()
    upper.addInt("key", Integer.MAX_VALUE)

    kuduContext.createTable(
      tableName,
      kuduSchema,
      new CreateTableOptions()
        .addHashPartitions(List("key").asJava, 2)
        .setRangePartitionColumns(List("key").asJava)
        .addRangePartition(lower, upper)
        .setNumReplicas(1)
    )
    kuduContext.insertRows(df, tableName)

    // now use new options to refer to the new table name
    val newOptions: Map[String, String] =
      Map("kudu.table" -> tableName, "kudu.master" -> harness.getMasterAddressesAsString)
    val checkDf = sqlContext.read.options(newOptions).kudu

    assert(checkDf.schema === df.schema)
    assertTrue(kuduContext.tableExists(tableName))
    assert(checkDf.count == 10)

    kuduContext.deleteTable(tableName)
    assertFalse(kuduContext.tableExists(tableName))
  }

  @Test
  def testInsertion() {
    val df = sqlContext.read.options(kuduOptions).kudu
    val changedDF = df
      .limit(1)
      .withColumn("key", df("key").plus(100))
      .withColumn("c2_s", lit("abc"))
    kuduContext.insertRows(changedDF, tableName)

    val newDF = sqlContext.read.options(kuduOptions).kudu
    val collected = newDF.filter("key = 100").collect()
    assertEquals("abc", collected(0).getAs[String]("c2_s"))

    deleteRow(100)
  }

  @Test
  def testInsertionMultiple() {
    val df = sqlContext.read.options(kuduOptions).kudu
    val changedDF = df
      .limit(2)
      .withColumn("key", df("key").plus(100))
      .withColumn("c2_s", lit("abc"))
    kuduContext.insertRows(changedDF, tableName)

    val newDF = sqlContext.read.options(kuduOptions).kudu
    val collected = newDF.filter("key = 100").collect()
    assertEquals("abc", collected(0).getAs[String]("c2_s"))

    val collectedTwo = newDF.filter("key = 101").collect()
    assertEquals("abc", collectedTwo(0).getAs[String]("c2_s"))

    deleteRow(100)
    deleteRow(101)
  }

  @Test
  def testInsertionIgnoreRows() {
    val df = sqlContext.read.options(kuduOptions).kudu
    val baseDF = df.limit(1) // filter down to just the first row

    // change the c2 string to abc and insert
    val updateDF = baseDF.withColumn("c2_s", lit("abc"))
    val kuduWriteOptions = KuduWriteOptions(ignoreDuplicateRowErrors = true)
    kuduContext.insertRows(updateDF, tableName, kuduWriteOptions)

    // change the key and insert
    val insertDF = df
      .limit(1)
      .withColumn("key", df("key").plus(100))
      .withColumn("c2_s", lit("def"))
    kuduContext.insertRows(insertDF, tableName, kuduWriteOptions)

    // read the data back
    val newDF = sqlContext.read.options(kuduOptions).kudu
    val collectedUpdate = newDF.filter("key = 0").collect()
    assertEquals("0", collectedUpdate(0).getAs[String]("c2_s"))
    val collectedInsert = newDF.filter("key = 100").collect()
    assertEquals("def", collectedInsert(0).getAs[String]("c2_s"))

    // restore the original state of the table
    deleteRow(100)
  }

  @Test
  def testInsertIgnoreRowsUsingDefaultSource() {
    val df = sqlContext.read.options(kuduOptions).kudu
    val baseDF = df.limit(1) // filter down to just the first row

    // change the c2 string to abc and insert
    val updateDF = baseDF.withColumn("c2_s", lit("abc"))
    val newOptions: Map[String, String] = Map(
      "kudu.table" -> tableName,
      "kudu.master" -> harness.getMasterAddressesAsString,
      "kudu.operation" -> "insert",
      "kudu.ignoreDuplicateRowErrors" -> "true")
    updateDF.write.options(newOptions).mode("append").kudu

    // change the key and insert
    val insertDF = df
      .limit(1)
      .withColumn("key", df("key").plus(100))
      .withColumn("c2_s", lit("def"))
    insertDF.write.options(newOptions).mode("append").kudu

    // read the data back
    val newDF = sqlContext.read.options(kuduOptions).kudu
    val collectedUpdate = newDF.filter("key = 0").collect()
    assertEquals("0", collectedUpdate(0).getAs[String]("c2_s"))
    val collectedInsert = newDF.filter("key = 100").collect()
    assertEquals("def", collectedInsert(0).getAs[String]("c2_s"))

    // restore the original state of the table
    deleteRow(100)
  }

  @Test
  def testInsertIgnoreRowsWriteOption() {
    val df = sqlContext.read.options(kuduOptions).kudu
    val baseDF = df.limit(1) // filter down to just the first row

    // change the c2 string to abc and insert
    val updateDF = baseDF.withColumn("c2_s", lit("abc"))
    val newOptions: Map[String, String] = Map(
      "kudu.table" -> tableName,
      "kudu.master" -> harness.getMasterAddressesAsString,
      "kudu.operation" -> "insert-ignore")
    updateDF.write.options(newOptions).mode("append").kudu

    // change the key and insert
    val insertDF = df
      .limit(1)
      .withColumn("key", df("key").plus(100))
      .withColumn("c2_s", lit("def"))
    insertDF.write.options(newOptions).mode("append").kudu

    // read the data back
    val newDF = sqlContext.read.options(kuduOptions).kudu
    val collectedUpdate = newDF.filter("key = 0").collect()
    assertEquals("0", collectedUpdate(0).getAs[String]("c2_s"))
    val collectedInsert = newDF.filter("key = 100").collect()
    assertEquals("def", collectedInsert(0).getAs[String]("c2_s"))

    // restore the original state of the table
    deleteRow(100)
  }

  @Test
  def testInsertIgnoreRowsMethod() {
    val df = sqlContext.read.options(kuduOptions).kudu
    val baseDF = df.limit(1) // filter down to just the first row

    // change the c2 string to abc and insert
    val updateDF = baseDF.withColumn("c2_s", lit("abc"))
    kuduContext.insertIgnoreRows(updateDF, tableName)

    // change the key and insert
    val insertDF = df
      .limit(1)
      .withColumn("key", df("key").plus(100))
      .withColumn("c2_s", lit("def"))
    kuduContext.insertIgnoreRows(insertDF, tableName)

    // read the data back
    val newDF = sqlContext.read.options(kuduOptions).kudu
    val collectedUpdate = newDF.filter("key = 0").collect()
    assertEquals("0", collectedUpdate(0).getAs[String]("c2_s"))
    val collectedInsert = newDF.filter("key = 100").collect()
    assertEquals("def", collectedInsert(0).getAs[String]("c2_s"))

    // restore the original state of the table
    deleteRow(100)
  }

  @Test
  def testUpsertRows() {
    val df = sqlContext.read.options(kuduOptions).kudu
    val baseDF = df.limit(1) // filter down to just the first row

    // change the c2 string to abc and update
    val updateDF = baseDF.withColumn("c2_s", lit("abc"))
    kuduContext.upsertRows(updateDF, tableName)

    // change the key and insert
    val insertDF = df
      .limit(1)
      .withColumn("key", df("key").plus(100))
      .withColumn("c2_s", lit("def"))
    kuduContext.upsertRows(insertDF, tableName)

    // read the data back
    val newDF = sqlContext.read.options(kuduOptions).kudu
    val collectedUpdate = newDF.filter("key = 0").collect()
    assertEquals("abc", collectedUpdate(0).getAs[String]("c2_s"))
    val collectedInsert = newDF.filter("key = 100").collect()
    assertEquals("def", collectedInsert(0).getAs[String]("c2_s"))

    // restore the original state of the table
    kuduContext.updateRows(baseDF.filter("key = 0").withColumn("c2_s", lit("0")), tableName)
    deleteRow(100)
  }

  @Test
  def testUpsertRowsIgnoreNulls() {
    val nonNullDF =
      sqlContext.createDataFrame(Seq((0, "foo"))).toDF("key", "val")
    kuduContext.insertRows(nonNullDF, simpleTableName)

    val dataDF = sqlContext.read
      .options(
        Map("kudu.master" -> harness.getMasterAddressesAsString, "kudu.table" -> simpleTableName))
      .kudu

    val nullDF = sqlContext
      .createDataFrame(Seq((0, null.asInstanceOf[String])))
      .toDF("key", "val")
    val ignoreNullOptions = KuduWriteOptions(ignoreNull = true)
    kuduContext.upsertRows(nullDF, simpleTableName, ignoreNullOptions)
    assert(dataDF.collect.toList === nonNullDF.collect.toList)

    val respectNullOptions = KuduWriteOptions(ignoreNull = false)
    kuduContext.updateRows(nonNullDF, simpleTableName)
    kuduContext.upsertRows(nullDF, simpleTableName, respectNullOptions)
    assert(dataDF.collect.toList === nullDF.collect.toList)

    kuduContext.updateRows(nonNullDF, simpleTableName)
    kuduContext.upsertRows(nullDF, simpleTableName)
    assert(dataDF.collect.toList === nullDF.collect.toList)

    val deleteDF = dataDF.filter("key = 0").select("key")
    kuduContext.deleteRows(deleteDF, simpleTableName)
  }

  @Test
  def testUpsertRowsIgnoreNullsUsingDefaultSource() {
    val nonNullDF =
      sqlContext.createDataFrame(Seq((0, "foo"))).toDF("key", "val")
    kuduContext.insertRows(nonNullDF, simpleTableName)

    val dataDF = sqlContext.read
      .options(
        Map("kudu.master" -> harness.getMasterAddressesAsString, "kudu.table" -> simpleTableName))
      .kudu

    val nullDF = sqlContext
      .createDataFrame(Seq((0, null.asInstanceOf[String])))
      .toDF("key", "val")
    val options_0: Map[String, String] = Map(
      "kudu.table" -> simpleTableName,
      "kudu.master" -> harness.getMasterAddressesAsString,
      "kudu.ignoreNull" -> "true")
    nullDF.write.options(options_0).mode("append").kudu
    assert(dataDF.collect.toList === nonNullDF.collect.toList)

    kuduContext.updateRows(nonNullDF, simpleTableName)
    val options_1: Map[String, String] =
      Map("kudu.table" -> simpleTableName, "kudu.master" -> harness.getMasterAddressesAsString)
    nullDF.write.options(options_1).mode("append").kudu
    assert(dataDF.collect.toList === nullDF.collect.toList)

    val deleteDF = dataDF.filter("key = 0").select("key")
    kuduContext.deleteRows(deleteDF, simpleTableName)
  }

  @Test
  def testDeleteRows() {
    val df = sqlContext.read.options(kuduOptions).kudu
    val deleteDF = df.filter("key = 0").select("key")
    kuduContext.deleteRows(deleteDF, tableName)

    // read the data back
    val newDF = sqlContext.read.options(kuduOptions).kudu
    val collectedDelete = newDF.filter("key = 0").collect()
    assertEquals(0, collectedDelete.length)

    // restore the original state of the table
    val insertDF = df.limit(1).filter("key = 0")
    kuduContext.insertRows(insertDF, tableName)
  }

  @Test
  def testOutOfOrderSelection() {
    val df =
      sqlContext.read.options(kuduOptions).kudu.select("c2_s", "c1_i", "key")
    val collected = df.collect()
    assert(collected(0).getString(0).equals("0"))
  }

  @Test
  def testTableNonFaultTolerantScan() {
    val results = sqlContext.sql(s"SELECT * FROM $tableName").collectAsList()
    assert(results.size() == rowCount)

    assert(!results.get(0).isNullAt(2))
    assert(results.get(1).isNullAt(2))
  }

  @Test
  def testTableFaultTolerantScan() {
    kuduOptions = Map(
      "kudu.table" -> tableName,
      "kudu.master" -> harness.getMasterAddressesAsString,
      "kudu.faultTolerantScan" -> "true")

    val table = "faultTolerantScanTest"
    sqlContext.read.options(kuduOptions).kudu.createOrReplaceTempView(table)
    val results = sqlContext.sql(s"SELECT * FROM $table").collectAsList()
    assert(results.size() == rowCount)

    assert(!results.get(0).isNullAt(2))
    assert(results.get(1).isNullAt(2))
  }

  @Test
  def testTableScanWithProjection() {
    assertEquals(10, sqlContext.sql(s"""SELECT key FROM $tableName""").count())
  }

  @Test
  def testTableScanWithProjectionAndPredicateDouble() {
    assertEquals(
      rows.count { case (key, i, s, ts) => i > 5 },
      sqlContext
        .sql(s"""SELECT key, c3_double FROM $tableName where c3_double > "5.0"""")
        .count())
  }

  @Test
  def testTableScanWithProjectionAndPredicateLong() {
    assertEquals(
      rows.count { case (key, i, s, ts) => i > 5 },
      sqlContext
        .sql(s"""SELECT key, c4_long FROM $tableName where c4_long > "5"""")
        .count())
  }

  @Test
  def testTableScanWithProjectionAndPredicateBool() {
    assertEquals(
      rows.count { case (key, i, s, ts) => i % 2 == 0 },
      sqlContext
        .sql(s"""SELECT key, c5_bool FROM $tableName where c5_bool = true""")
        .count())
  }

  @Test
  def testTableScanWithProjectionAndPredicateShort() {
    assertEquals(
      rows.count { case (key, i, s, ts) => i > 5 },
      sqlContext
        .sql(s"""SELECT key, c6_short FROM $tableName where c6_short > 5""")
        .count())

  }

  @Test
  def testTableScanWithProjectionAndPredicateFloat() {
    assertEquals(
      rows.count { case (key, i, s, ts) => i > 5 },
      sqlContext
        .sql(s"""SELECT key, c7_float FROM $tableName where c7_float > 5""")
        .count())

  }

  @Test
  def testTableScanWithProjectionAndPredicateDecimal32() {
    assertEquals(
      rows.count { case (key, i, s, ts) => i > 5 },
      sqlContext
        .sql(s"""SELECT key, c11_decimal32 FROM $tableName where c11_decimal32 > 5""")
        .count())
  }

  @Test
  def testTableScanWithProjectionAndPredicateDecimal64() {
    assertEquals(
      rows.count { case (key, i, s, ts) => i > 5 },
      sqlContext
        .sql(s"""SELECT key, c12_decimal64 FROM $tableName where c12_decimal64 > 5""")
        .count())
  }

  @Test
  def testTableScanWithProjectionAndPredicateDecimal128() {
    assertEquals(
      rows.count { case (key, i, s, ts) => i > 5 },
      sqlContext
        .sql(s"""SELECT key, c13_decimal128 FROM $tableName where c13_decimal128 > 5""")
        .count())
  }

  @Test
  def testTableScanWithProjectionAndPredicate() {
    assertEquals(
      rows.count { case (key, i, s, ts) => s != null && s > "5" },
      sqlContext
        .sql(s"""SELECT key FROM $tableName where c2_s > "5"""")
        .count())

    assertEquals(
      rows.count { case (key, i, s, ts) => s != null },
      sqlContext
        .sql(s"""SELECT key, c2_s FROM $tableName where c2_s IS NOT NULL""")
        .count())
  }

  @Test
  def testBasicSparkSQL() {
    val results = sqlContext.sql("SELECT * FROM " + tableName).collectAsList()
    assert(results.size() == rowCount)

    assert(results.get(1).isNullAt(2))
    assert(!results.get(0).isNullAt(2))
  }

  @Test
  def testBasicSparkSQLWithProjection() {
    val results = sqlContext.sql("SELECT key FROM " + tableName).collectAsList()
    assert(results.size() == rowCount)
    assert(results.get(0).size.equals(1))
    assert(results.get(0).getInt(0).equals(0))
  }

  @Test
  def testBasicSparkSQLWithPredicate() {
    val results = sqlContext
      .sql("SELECT key FROM " + tableName + " where key=1")
      .collectAsList()
    assert(results.size() == 1)
    assert(results.get(0).size.equals(1))
    assert(results.get(0).getInt(0).equals(1))

  }

  @Test
  def testBasicSparkSQLWithTwoPredicates() {
    val results = sqlContext
      .sql("SELECT key FROM " + tableName + " where key=2 and c2_s='2'")
      .collectAsList()
    assert(results.size() == 1)
    assert(results.get(0).size.equals(1))
    assert(results.get(0).getInt(0).equals(2))
  }

  @Test
  def testBasicSparkSQLWithInListPredicate() {
    val keys = Array(1, 5, 7)
    val results = sqlContext
      .sql(s"SELECT key FROM $tableName where key in (${keys.mkString(", ")})")
      .collectAsList()
    assert(results.size() == keys.length)
    keys.zipWithIndex.foreach {
      case (v, i) =>
        assert(results.get(i).size.equals(1))
        assert(results.get(i).getInt(0).equals(v))
    }
  }

  @Test
  def testBasicSparkSQLWithInListPredicateOnString() {
    val keys = Array(1, 4, 6)
    val results = sqlContext
      .sql(s"SELECT key FROM $tableName where c2_s in (${keys.mkString("'", "', '", "'")})")
      .collectAsList()
    assert(results.size() == keys.count(_ % 2 == 0))
    keys.filter(_ % 2 == 0).zipWithIndex.foreach {
      case (v, i) =>
        assert(results.get(i).size.equals(1))
        assert(results.get(i).getInt(0).equals(v))
    }
  }

  @Test
  def testBasicSparkSQLWithInListAndComparisonPredicate() {
    val keys = Array(1, 5, 7)
    val results = sqlContext
      .sql(s"SELECT key FROM $tableName where key>2 and key in (${keys.mkString(", ")})")
      .collectAsList()
    assert(results.size() == keys.count(_ > 2))
    keys.filter(_ > 2).zipWithIndex.foreach {
      case (v, i) =>
        assert(results.get(i).size.equals(1))
        assert(results.get(i).getInt(0).equals(v))
    }
  }

  @Test
  def testBasicSparkSQLWithTwoPredicatesNegative() {
    val results = sqlContext
      .sql("SELECT key FROM " + tableName + " where key=1 and c2_s='2'")
      .collectAsList()
    assert(results.size() == 0)
  }

  @Test
  def testBasicSparkSQLWithTwoPredicatesIncludingString() {
    val results = sqlContext
      .sql("SELECT key FROM " + tableName + " where c2_s='2'")
      .collectAsList()
    assert(results.size() == 1)
    assert(results.get(0).size.equals(1))
    assert(results.get(0).getInt(0).equals(2))
  }

  @Test
  def testBasicSparkSQLWithTwoPredicatesAndProjection() {
    val results = sqlContext
      .sql("SELECT key, c2_s FROM " + tableName + " where c2_s='2'")
      .collectAsList()
    assert(results.size() == 1)
    assert(results.get(0).size.equals(2))
    assert(results.get(0).getInt(0).equals(2))
    assert(results.get(0).getString(1).equals("2"))
  }

  @Test
  def testBasicSparkSQLWithTwoPredicatesGreaterThan() {
    val results = sqlContext
      .sql("SELECT key, c2_s FROM " + tableName + " where c2_s>='2'")
      .collectAsList()
    assert(results.size() == 4)
    assert(results.get(0).size.equals(2))
    assert(results.get(0).getInt(0).equals(2))
    assert(results.get(0).getString(1).equals("2"))
  }

  @Test
  def testSparkSQLStringStartsWithFilters() {
    // This test requires a special table.
    val testTableName = "startswith"
    val schema = new Schema(
      List(new ColumnSchemaBuilder("key", Type.STRING).key(true).build()).asJava)
    val tableOptions = new CreateTableOptions()
      .setRangePartitionColumns(List("key").asJava)
      .setNumReplicas(1)
    val testTable = kuduClient.createTable(testTableName, schema, tableOptions)

    val kuduSession = kuduClient.newSession()
    val chars = List('a', 'b', '乕', Char.MaxValue, '\u0000')
    val keys = for {
      x <- chars
      y <- chars
      z <- chars
      w <- chars
    } yield Array(x, y, z, w).mkString
    keys.foreach { key =>
      val insert = testTable.newInsert
      val row = insert.getRow
      val r = Array(1, 2, 3)
      row.addString(0, key)
      kuduSession.apply(insert)
    }
    val options: Map[String, String] =
      Map("kudu.table" -> testTableName, "kudu.master" -> harness.getMasterAddressesAsString)
    sqlContext.read.options(options).kudu.createOrReplaceTempView(testTableName)

    val checkPrefixCount = { prefix: String =>
      val results = sqlContext.sql(s"SELECT key FROM $testTableName WHERE key LIKE '$prefix%'")
      assertEquals(keys.count(k => k.startsWith(prefix)), results.count())
    }
    // empty string
    checkPrefixCount("")
    // one character
    for (x <- chars) {
      checkPrefixCount(Array(x).mkString)
    }
    // all two character combos
    for {
      x <- chars
      y <- chars
    } {
      checkPrefixCount(Array(x, y).mkString)
    }
  }

  @Test
  def testSparkSQLIsNullPredicate() {
    var results = sqlContext
      .sql("SELECT key FROM " + tableName + " where c2_s IS NULL")
      .collectAsList()
    assert(results.size() == 5)

    results = sqlContext
      .sql("SELECT key FROM " + tableName + " where key IS NULL")
      .collectAsList()
    assert(results.isEmpty())
  }

  @Test
  def testSparkSQLIsNotNullPredicate() {
    var results = sqlContext
      .sql("SELECT key FROM " + tableName + " where c2_s IS NOT NULL")
      .collectAsList()
    assert(results.size() == 5)

    results = sqlContext
      .sql("SELECT key FROM " + tableName + " where key IS NOT NULL")
      .collectAsList()
    assert(results.size() == 10)
  }

  @Test
  def testSQLInsertInto() {
    val insertTable = "insertintotest"

    // read 0 rows just to get the schema
    val df = sqlContext.sql(s"SELECT * FROM $tableName LIMIT 0")
    kuduContext.createTable(
      insertTable,
      df.schema,
      Seq("key"),
      new CreateTableOptions()
        .setRangePartitionColumns(List("key").asJava)
        .setNumReplicas(1))

    val newOptions: Map[String, String] =
      Map("kudu.table" -> insertTable, "kudu.master" -> harness.getMasterAddressesAsString)
    sqlContext.read
      .options(newOptions)
      .kudu
      .createOrReplaceTempView(insertTable)

    sqlContext.sql(s"INSERT INTO TABLE $insertTable SELECT * FROM $tableName")
    val results =
      sqlContext.sql(s"SELECT key FROM $insertTable").collectAsList()
    assertEquals(10, results.size())
  }

  @Test
  def testSQLInsertOverwriteUnsupported() {
    val insertTable = "insertoverwritetest"

    // read 0 rows just to get the schema
    val df = sqlContext.sql(s"SELECT * FROM $tableName LIMIT 0")
    kuduContext.createTable(
      insertTable,
      df.schema,
      Seq("key"),
      new CreateTableOptions()
        .setRangePartitionColumns(List("key").asJava)
        .setNumReplicas(1))

    val newOptions: Map[String, String] =
      Map("kudu.table" -> insertTable, "kudu.master" -> harness.getMasterAddressesAsString)
    sqlContext.read
      .options(newOptions)
      .kudu
      .createOrReplaceTempView(insertTable)

    try {
      sqlContext.sql(s"INSERT OVERWRITE TABLE $insertTable SELECT * FROM $tableName")
      fail("insert overwrite should throw UnsupportedOperationException")
    } catch {
      case _: UnsupportedOperationException => // good
      case NonFatal(_) =>
        fail("insert overwrite should throw UnsupportedOperationException")
    }
  }

  @Test
  def testWriteUsingDefaultSource() {
    val df = sqlContext.read.options(kuduOptions).kudu

    val newTable = "testwritedatasourcetable"
    kuduContext.createTable(
      newTable,
      df.schema,
      Seq("key"),
      new CreateTableOptions()
        .setRangePartitionColumns(List("key").asJava)
        .setNumReplicas(1))

    val newOptions: Map[String, String] =
      Map("kudu.table" -> newTable, "kudu.master" -> harness.getMasterAddressesAsString)
    df.write.options(newOptions).mode("append").kudu

    val checkDf = sqlContext.read.options(newOptions).kudu
    assert(checkDf.schema === df.schema)
    assertTrue(kuduContext.tableExists(newTable))
    assert(checkDf.count == 10)
  }

  @Test
  def testCreateRelationWithSchema() {
    // user-supplied schema that is compatible with actual schema, but with the key at the end
    val userSchema: StructType = StructType(
      List(
        StructField("c4_long", DataTypes.LongType),
        StructField("key", DataTypes.IntegerType)
      ))

    val dfDefaultSchema = sqlContext.read.options(kuduOptions).kudu
    assertEquals(14, dfDefaultSchema.schema.fields.length)

    val dfWithUserSchema =
      sqlContext.read.options(kuduOptions).schema(userSchema).kudu
    assertEquals(2, dfWithUserSchema.schema.fields.length)

    dfWithUserSchema.limit(10).collect()
    assertTrue(dfWithUserSchema.columns.deep == Array("c4_long", "key").deep)
  }

  @Test
  def testCreateRelationWithInvalidSchema() {
    // user-supplied schema that is NOT compatible with actual schema
    val userSchema: StructType = StructType(
      List(
        StructField("foo", DataTypes.LongType),
        StructField("bar", DataTypes.IntegerType)
      ))

    intercept[IllegalArgumentException] {
      sqlContext.read.options(kuduOptions).schema(userSchema).kudu
    }.getMessage should include("Unknown column: foo")
  }

  @Test
  def testScanLocality() {
    kuduOptions = Map(
      "kudu.table" -> tableName,
      "kudu.master" -> harness.getMasterAddressesAsString,
      "kudu.scanLocality" -> "closest_replica")

    val table = "scanLocalityTest"
    sqlContext.read.options(kuduOptions).kudu.createOrReplaceTempView(table)
    val results = sqlContext.sql(s"SELECT * FROM $table").collectAsList()
    assert(results.size() == rowCount)

    assert(!results.get(0).isNullAt(2))
    assert(results.get(1).isNullAt(2))
  }

  // Verify that the propagated timestamp is properly updated inside
  // the same client.
  @Test
  def testTimestampPropagation() {
    val df = sqlContext.read.options(kuduOptions).kudu
    val insertDF = df
      .limit(1)
      .withColumn(
        "key",
        df("key")
          .plus(100))
      .withColumn("c2_s", lit("abc"))

    // Initiate a write via KuduContext, and verify that the client should
    // have propagated timestamp.
    kuduContext.insertRows(insertDF, tableName)
    assert(kuduContext.syncClient.getLastPropagatedTimestamp > 0)
    var prevTimestamp = kuduContext.syncClient.getLastPropagatedTimestamp

    // Initiate a read via DataFrame, and verify that the client should
    // move the propagated timestamp further.
    val newDF = sqlContext.read.options(kuduOptions).kudu
    val collected = newDF.filter("key = 100").collect()
    assertEquals("abc", collected(0).getAs[String]("c2_s"))
    assert(kuduContext.syncClient.getLastPropagatedTimestamp > prevTimestamp)
    prevTimestamp = kuduContext.syncClient.getLastPropagatedTimestamp

    // Initiate a read via KuduContext, and verify that the client should
    // move the propagated timestamp further.
    val rdd = kuduContext.kuduRDD(ss.sparkContext, tableName, List("key"))
    assert(rdd.collect.length == 11)
    assert(kuduContext.syncClient.getLastPropagatedTimestamp > prevTimestamp)
    prevTimestamp = kuduContext.syncClient.getLastPropagatedTimestamp

    // Initiate another write via KuduContext, and verify that the client should
    // move the propagated timestamp further.
    val updateDF = df
      .limit(1)
      .withColumn(
        "key",
        df("key")
          .plus(100))
      .withColumn("c2_s", lit("def"))
    val kuduWriteOptions = KuduWriteOptions(ignoreDuplicateRowErrors = true)
    kuduContext.insertRows(updateDF, tableName, kuduWriteOptions)
    assert(kuduContext.syncClient.getLastPropagatedTimestamp > prevTimestamp)
  }

  /**
   * Assuming that the only part of the logical plan is a Kudu scan, this
   * function extracts the KuduRelation from the passed DataFrame for
   * testing purposes.
   */
  def kuduRelationFromDataFrame(dataFrame: DataFrame) = {
    val logicalPlan = dataFrame.queryExecution.logical
    val logicalRelation = logicalPlan.asInstanceOf[LogicalRelation]
    val baseRelation = logicalRelation.relation
    baseRelation.asInstanceOf[KuduRelation]
  }

  /**
   * Verify that the kudu.scanRequestTimeoutMs parameter is parsed by the
   * DefaultSource and makes it into the KuduRelation as a configuration
   * parameter.
   */
  @Test
  def testScanRequestTimeoutPropagation() {
    kuduOptions = Map(
      "kudu.table" -> tableName,
      "kudu.master" -> harness.getMasterAddressesAsString,
      "kudu.scanRequestTimeoutMs" -> "1")
    val dataFrame = sqlContext.read.options(kuduOptions).kudu
    val kuduRelation = kuduRelationFromDataFrame(dataFrame)
    assert(kuduRelation.readOptions.scanRequestTimeoutMs == Some(1))
  }

  /**
   * Verify that the kudu.socketReadTimeoutMs parameter is parsed by the
   * DefaultSource and makes it into the KuduRelation as a configuration
   * parameter.
   */
  @Test
  def testSocketReadTimeoutPropagation() {
    kuduOptions = Map(
      "kudu.table" -> tableName,
      "kudu.master" -> harness.getMasterAddressesAsString,
      "kudu.socketReadTimeoutMs" -> "1")
    val dataFrame = sqlContext.read.options(kuduOptions).kudu
    val kuduRelation = kuduRelationFromDataFrame(dataFrame)
    assert(kuduRelation.readOptions.socketReadTimeoutMs == Some(1))
  }
}
