#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

from __future__ import division

from kudu.compat import unittest
from kudu.tests.util import TestScanBase
from kudu.tests.common import KuduTestBase, TimeoutError
import kudu
import datetime
import time
import pytest


class TestScanner(TestScanBase):

    @classmethod
    def setUpClass(self):
        super(TestScanner, self).setUpClass()

    def setUp(self):
        pass

    def test_scan_rows_basic(self):
        # Let's scan with no predicates
        scanner = self.table.scanner().open()

        tuples = scanner.read_all_tuples()
        self.assertEqual(sorted(tuples), self.tuples)

    def test_scan_rows_simple_predicate(self):
        key = self.table['key']
        preds = [key > 19, key < 50]

        def _read_predicates(preds):
            scanner = self.table.scanner()
            scanner.add_predicates(preds)
            scanner.open()
            return scanner.read_all_tuples()

        tuples = _read_predicates(preds)
        self.assertEqual(sorted(tuples), self.tuples[20:50])

        # verify predicates reusable
        tuples = _read_predicates(preds)
        self.assertEqual(sorted(tuples), self.tuples[20:50])

    def test_scan_limit(self):
        # Set limits both below and above the max number of rows.
        limits = [self.nrows - 1, self.nrows, self.nrows + 1]
        for limit in limits:
            scanner = self.table.scanner()
            scanner.set_limit(limit)
            tuples = scanner.read_all_tuples()
            self.assertEqual(len(tuples), min(limit, self.nrows))

    def test_scan_rows_string_predicate_and_projection(self):
        scanner = self.table.scanner()
        scanner.set_projected_column_names(['key', 'string_val'])
        sv = self.table['string_val']
        scanner.add_predicates([sv >= 'hello_20',
                                sv <= 'hello_22'])
        scanner.set_fault_tolerant()
        scanner.open()

        tuples = scanner.read_all_tuples()

        self.assertEqual(sorted(tuples), [(20, 'hello_20'), (22, 'hello_22')])

    def test_scan_rows_in_list_predicate(self):
        """
        Test scanner with an InList predicate and
        a string comparison predicate
        """
        key_list = [2, 98]
        scanner = self.table.scanner()
        scanner.set_fault_tolerant()\
            .add_predicates([
                self.table[0].in_list(key_list),
                self.table['string_val'] >= 'hello_9'
            ])
        scanner.open()

        tuples = scanner.read_all_tuples()

        self.assertEqual(tuples, [self.tuples[98]])

    def test_scan_rows_is_not_null_predicate(self):
        """
        Test scanner with an IsNotNull predicate on string_val column
        """
        pred = self.table['string_val'].is_not_null()
        scanner = self.table.scanner()
        scanner.add_predicate(pred)
        scanner.open()

        tuples = scanner.read_all_tuples()

        rows = [i for i in range(100) if i % 2 == 0]

        self.assertEqual(sorted(tuples), [self.tuples[i] for i in rows])

    def test_scan_rows_is_null_predicate(self):
        """
        Test scanner with an IsNull predicate on string_val column
        """
        pred = self.table['string_val'].is_null()
        scanner = self.table.scanner()
        scanner.add_predicate(pred)
        scanner.open()

        tuples = scanner.read_all_tuples()

        rows = [i for i in range(100) if i % 2 != 0]

        self.assertEqual(sorted(tuples), [self.tuples[i] for i in rows])

    def test_index_projection_with_schema(self):
        scanner = self.table.scanner()
        scanner.set_projected_column_indexes([0, 1])

        scanner.set_fault_tolerant()
        scanner.open()

        tuples = scanner.read_all_tuples()

        # Build schema to check against
        builder = kudu.schema_builder()
        builder.add_column('key', kudu.int32, nullable=False)
        builder.add_column('int_val', kudu.int32)
        builder.set_primary_keys(['key'])
        expected_schema = builder.build()

        # Build new schema from projection schema
        builder = kudu.schema_builder()
        for col in scanner.get_projection_schema():
            builder.copy_column(col)
        builder.set_primary_keys(['key'])
        new_schema = builder.build()

        self.assertEqual(tuples, [t[0:2] for t in self.tuples])
        self.assertTrue(expected_schema.equals(new_schema))

    def test_scan_with_bounds(self):
        scanner = self.table.scanner()
        scanner.set_fault_tolerant()\
            .add_lower_bound({'key': 50})\
            .add_exclusive_upper_bound({'key': 55})
        scanner.open()

        tuples = scanner.read_all_tuples()

        self.assertEqual(sorted(tuples), self.tuples[50:55])

    def test_scan_invalid_predicates(self):
        scanner = self.table.scanner()
        sv = self.table['string_val']

        with self.assertRaises(TypeError):
            scanner.add_predicates([sv >= None])

        with self.assertRaises(TypeError):
            scanner.add_predicates([sv >= 1])

        with self.assertRaises(TypeError):
            scanner.add_predicates([sv.in_list(['testing',
                                                datetime.datetime.utcnow()])])

        with self.assertRaises(TypeError):
            scanner.add_predicates([sv.in_list([
                'hello_20',
                120
            ])])

    def test_scan_batch_by_batch(self):
        scanner = self.table.scanner()
        scanner.set_fault_tolerant()
        lower_bound = scanner.new_bound()
        lower_bound['key'] = 10
        scanner.add_lower_bound(lower_bound)
        upper_bound = scanner.new_bound()
        upper_bound['key'] = 90
        scanner.add_exclusive_upper_bound(upper_bound)
        scanner.open()

        tuples = []
        while scanner.has_more_rows():
            batch = scanner.next_batch()
            tuples.extend(batch.as_tuples())

        self.assertEqual(sorted(tuples), self.tuples[10:90])

    def test_unixtime_micros(self):
        """
        Test setting and getting unixtime_micros fields
        """
        # Insert new rows
        self.insert_new_unixtime_micros_rows()

        # Validate results
        scanner = self.table.scanner()
        scanner.set_fault_tolerant().open()
        self.assertEqual(sorted(self.tuples), scanner.read_all_tuples())

    def test_read_mode(self):
        """
        Test scanning in latest, snapshot and read_your_writes read modes.
        """
        # Delete row
        self.delete_insert_row_for_read_test()

        # Check scanner results prior to delete
        scanner = self.table.scanner()
        scanner.set_read_mode('snapshot')\
            .set_snapshot(self.snapshot_timestamp)\
            .open()

        self.assertEqual(sorted(self.tuples[1:]), sorted(scanner.read_all_tuples()))

        # Check scanner results after delete with latest mode
        timeout = time.time() + 10
        check_tuples = []
        while check_tuples != sorted(self.tuples):
            if time.time() > timeout:
                raise TimeoutError("Could not validate results in allocated" +
                                   "time.")

            scanner = self.table.scanner()
            scanner.set_read_mode(kudu.READ_LATEST)\
                .open()
            check_tuples = sorted(scanner.read_all_tuples())
            # Avoid tight looping
            time.sleep(0.05)

        # Check scanner results after delete with read_your_writes mode
        scanner = self.table.scanner()
        scanner.set_read_mode('read_your_writes')\
            .open()

        self.assertEqual(sorted(self.tuples), sorted(scanner.read_all_tuples()))

    def test_resource_metrics_and_cache_blocks(self):
        """
        Test getting the resource metrics after scanning and
        setting the scanner to not cache blocks.
        """

        # Build scanner and read through all batches and retrieve metrics.
        scanner = self.table.scanner()
        scanner.set_fault_tolerant().set_cache_blocks(False).open()
        scanner.read_all_tuples()
        metrics = scanner.get_resource_metrics()

        # Confirm that the scanner returned cache hit and miss values.
        self.assertTrue('cfile_cache_hit_bytes' in metrics)
        self.assertTrue('cfile_cache_miss_bytes' in metrics)

    def verify_pred_type_scans(self, preds, row_indexes, count_only=False):
        # Using the incoming list of predicates, verify that the row returned
        # matches the inserted tuple at the row indexes specified in a
        # slice object
        scanner = self.type_table.scanner()
        scanner.set_fault_tolerant()
        scanner.add_predicates(preds)
        scanner.set_projected_column_names(self.projected_names_w_o_float)
        tuples = scanner.open().read_all_tuples()

        # verify rows
        if count_only:
            self.assertEqual(len(self.type_test_rows[row_indexes]), len(tuples))
        else:
            self.assertEqual(sorted(self.type_test_rows[row_indexes]), tuples)

    def test_unixtime_micros_pred(self):
        # Test unixtime_micros value predicate
        self._test_unixtime_micros_pred()

    def test_bool_pred(self):
        # Test a boolean value predicate
        self._test_bool_pred()

    def test_double_pred(self):
        # Test a double precision float predicate
        self._test_double_pred()

    def test_float_pred(self):
        # Test a single precision float predicate
        # Does a row check count only
        self._test_float_pred()

    def test_decimal_pred(self):
        if kudu.CLIENT_SUPPORTS_DECIMAL:
            # Test a decimal predicate
            self._test_decimal_pred()

    def test_binary_pred(self):
        # Test a binary predicate
        self._test_binary_pred()

    def test_scan_selection(self):
        """
        This test confirms that setting the scan selection policy on the
        scanner does not cause any errors. There is no way to confirm
        that the policy was actually set. This functionality is
        tested in the C++ test:
            ClientTest.TestReplicatedMultiTabletTableFailover.
        """

        for policy in ['leader', kudu.CLOSEST_REPLICA, 2]:
            scanner = self.table.scanner()
            scanner.set_selection(policy)
            scanner.open()
            self.assertEqual(sorted(scanner.read_all_tuples()),
                             sorted(self.tuples))

    @pytest.mark.skipif(not (kudu.CLIENT_SUPPORTS_PANDAS),
                        reason="Pandas required to run this test.")
    def test_scanner_to_pandas_types(self):
        """
        This test confirms that data types are converted as expected to Pandas.
        """
        import numpy as np
        scanner = self.type_table.scanner()
        df = scanner.to_pandas()
        types = df.dtypes

        if kudu.CLIENT_SUPPORTS_DECIMAL:
            self.assertEqual(types[0], np.int64)
            self.assertEqual(types[1], 'datetime64[ns, UTC]')
            self.assertEqual(types[2], np.object)
            self.assertEqual(types[3], np.object)
            self.assertEqual(types[4], np.bool)
            self.assertEqual(types[5], np.float64)
            self.assertEqual(types[6], np.int8)
            self.assertEqual(types[7], np.object)
            self.assertEqual(types[8], np.float32)
        else:
            self.assertEqual(types[0], np.int64)
            self.assertEqual(types[1], 'datetime64[ns, UTC]')
            self.assertEqual(types[2], np.object)
            self.assertEqual(types[3], np.bool)
            self.assertEqual(types[4], np.float64)
            self.assertEqual(types[5], np.int8)
            self.assertEqual(types[6], np.object)
            self.assertEqual(types[7], np.float32)

    @pytest.mark.skipif(not (kudu.CLIENT_SUPPORTS_PANDAS),
                        reason="Pandas required to run this test.")
    def test_scanner_to_pandas_row_count(self):
        """
        This test confirms that the record counts match between Pandas and the scanner.
        """
        scanner = self.type_table.scanner()
        scanner_count = len(scanner.read_all_tuples())
        scanner = self.type_table.scanner()
        df = scanner.to_pandas()
        self.assertEqual(scanner_count, df.shape[0])

    @pytest.mark.skipif(not (kudu.CLIENT_SUPPORTS_PANDAS),
                        reason="Pandas required to run this test.")
    def test_scanner_to_pandas_index(self):
        """
        This test confirms that an index is correctly applied.
        """
        scanner = self.type_table.scanner()
        df = scanner.to_pandas(index='key')
        self.assertEqual(df.index.name, 'key')
        self.assertEqual(list(df.index), [1, 2])

    @pytest.mark.skipif((not(kudu.CLIENT_SUPPORTS_PANDAS) or
                        (not(kudu.CLIENT_SUPPORTS_DECIMAL))),
                        reason="Pandas and Decimal support required to run this test.")
    def test_scanner_to_pandas_index(self):
        """
        This test confirms that a decimal column is coerced to a double when specified.
        """
        import numpy as np
        scanner = self.type_table.scanner()
        df = scanner.to_pandas(coerce_float=True)
        types = df.dtypes
        self.assertEqual(types[2], np.float64)
