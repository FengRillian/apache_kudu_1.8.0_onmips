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

from kudu.client import (Client, Table, Scanner, Session,  # noqa
                         Insert, Update, Delete, Predicate,
                         TimeDelta, KuduError, ScanTokenBuilder,
                         ScanToken,
                         LEADER_ONLY,
                         CLOSEST_REPLICA,
                         FIRST_REPLICA,
                         FLUSH_AUTO_BACKGROUND,
                         FLUSH_AUTO_SYNC,
                         FLUSH_MANUAL,
                         READ_LATEST,
                         READ_AT_SNAPSHOT,
                         READ_YOUR_WRITES,
                         EXCLUSIVE_BOUND,
                         INCLUSIVE_BOUND,
                         CLIENT_SUPPORTS_DECIMAL,
                         CLIENT_SUPPORTS_PANDAS)

from kudu.errors import (KuduException, KuduBadStatus, KuduNotFound,  # noqa
                         KuduNotSupported,
                         KuduInvalidArgument)

from kudu.schema import (int8, int16, int32, int64, string_ as string,  # noqa
                         double_ as double, float_, float_ as float, binary,
                         unixtime_micros, bool_ as bool, decimal,
                         KuduType,
                         SchemaBuilder, ColumnSpec, Schema, ColumnSchema,
                         COMPRESSION_DEFAULT,
                         COMPRESSION_NONE,
                         COMPRESSION_SNAPPY,
                         COMPRESSION_LZ4,
                         COMPRESSION_ZLIB,
                         ENCODING_AUTO,
                         ENCODING_PLAIN,
                         ENCODING_PREFIX,
                         ENCODING_BIT_SHUFFLE,
                         ENCODING_RLE,
                         ENCODING_DICT)


def connect(host, port=7051, admin_timeout_ms=None, rpc_timeout_ms=None):
    """
    Connect to a Kudu master server

    Parameters
    ----------
    host : string/list
      Server address of master or a list of addresses
    port : int/list, optional, default 7051
      Server port or list of ports. If a list of addresses is provided and
      only a single port, that port will be used for all master addresses.
    admin_timeout_ms : int, optional
      Admin timeout in milliseconds
    rpc_timeout_ms : int, optional
      RPC timeout in milliseconds

    Returns
    -------
    client : kudu.Client
    """
    addresses = []
    if isinstance(host, list):
        if isinstance(port, list):
            if len(host) == len(port):
                for h, p in zip(host, port):
                    addresses.append('{0}:{1}'.format(h, p))
            else:
                raise ValueError("Host and port lists are not of equal length.")
        else:
            for h in host:
                addresses.append('{0}:{1}'.format(h, port))
    else:
        if isinstance(port, list):
            raise ValueError("List of ports provided but only a single host.")
        else:
            addresses.append('{0}:{1}'.format(host, port))

    return Client(addresses, admin_timeout_ms=admin_timeout_ms,
                  rpc_timeout_ms=rpc_timeout_ms)


def timedelta(seconds=0, millis=0, micros=0, nanos=0):
    """
    Construct a Kudu TimeDelta to set timeouts, etc. Use this function instead
    of interacting with the TimeDelta class yourself.

    Returns
    -------
    delta : kudu.client.TimeDelta
    """
    from kudu.compat import long
    # TimeDelta is a wrapper for kudu::MonoDelta
    total_ns = (long(0) + seconds * long(1000000000) +
                millis * long(1000000) + micros * long(1000) + nanos)
    return TimeDelta.from_nanos(total_ns)


def schema_builder():
    """
    Create a kudu.SchemaBuilder instance

    Examples
    --------
    builder = kudu.schema_builder()
    builder.add_column('key1', kudu.int64, nullable=False)
    builder.add_column('key2', kudu.int32, nullable=False)

    (builder.add_column('name', kudu.string)
     .nullable()
     .compression('lz4'))

    builder.add_column('value1', kudu.double)
    builder.add_column('value2', kudu.int8, encoding='rle')
    builder.set_primary_keys(['key1', 'key2'])

    schema = builder.build()

    Returns
    -------
    builder : SchemaBuilder
    """
    return SchemaBuilder()


from .version import version as __version__  # noqa
