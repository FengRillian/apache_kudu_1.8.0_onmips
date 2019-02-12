<!--
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

Schema Changes
============================================================

Column IDs
------------------------------
Internal to a Schema, and not exposed to the user, each column in a schema has
a unique identifier. The identifiers are integers which are not re-used,
and serve to distinguish an old column from a new one in the case that they
have the same name.

For example:

```
> CREATE TABLE x (col_a int, col_b int);
> INSERT INTO x VALUES (1, 1);
> ALTER TABLE x DROP COLUMN col_b;
> ALTER TABLE x ADD COLUMN col_b int not null default 999;
```

In this case, although the Schema at the end of the sequence looks the same
as the one at the beginning, the correct data is:

```
> SELECT * from x;
 col_a   | col_b
------------------
  1      | 999
```

In other words, we cannot re-materialize data from the old `col_b` into the new
`col_b`.

If we were to dump the initial schema and the new schema, we would see that although
the two `col_b`s have the same name, they would have different column IDs.

Column IDs are internal to the server and not sent by the user on RPCs. Clients
specify columns by name. This is because we expect a client to continue to make
queries like "`select sum(col_b) from x;`" without any refresh of the schema, even
if the column is dropped and re-added with new data.

Schemas specified in RPCs
------------------------------

When the user makes an RPC to read or write from a tablet, the RPC specifies only
the names, types, and nullability of the columns. Internal to the server, we map
the names to the internal IDs.

If the user specifies a column name which does not exist in the latest schema,
it is considered an error.

If the type or nullability does not match, we also currently consider it an error.
In the future, we may be able to adapt the data to the requested type (eg promote
smaller to larger integers on read, promote non-null data to a nullable read, etc).

Handling varying schemas at read time
------------------------------

```
 + Tablet
 |---- MemRowSet
 |---- DiskRowSet N
 |-------- CFileSet
 |-------- Delta Tracker
 |------------ Delta Memstore
 |------------ Delta File N
```

Because the Schema of a table may change over time, different rowsets may have
been written with different schemas. At read time, the server determines a Schema
for the read based on the current metadata of the tablet. This Schema determines
what to do as the read path encounters older data which was inserted prior to
the schema change and thus may be  missing some columns.

For each column in the read schema which is not present in the data, that column
may be treated in one of two ways:

1. In the case that the new column has a "read default" in the metadata, that
   value is materialized for each cell.
2. If no "read default" is present, then the column must be nullable. In that
   case, a column of NULLs is materialized.

Currently, Kudu does not handle type changes. In the future, we may also need to
add type adapters to convert older data to the new type.

When reading delta files, updates to columns which have since been removed are
ignored. Updates to new columns are applied on top of the materialized default
column data.

Compaction
------------------------------
Each CFileSet and DeltaFile has a schema associated to describe the data in it.
On compaction, CFileSet/DeltaFiles with different schemas may be aggregated into a new file.
This new file will have the latest schema and all the rows must be projected.

In the case of CFiles, the projection affects only the new columns, where the read default
value will be written as data, or in case of "alter type" where the "encoding" is changed.

In the case of DeltaFiles, the projection is essential since the RowChangeList is serialized
with no hint of the schema used. This means that you can read a RowChangeList only if you
know the exact serialization schema.
