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

Maintenance Op Scheduling
===============================================================================

For the purpose of this document, "maintenance operations" are any background
processes that Kudu runs in the course of normal operation.  The
MaintenanceManager must schedule these operations intelligently to keep the
system operating smoothly.  Partly, this is a tradeoff between current
performance and future performance.  For example, running a compaction will
spend some I/O now in order to speed up insertions later.  Partly, this is a
matter of performing necessary tasks that, if left undone, would compromise the
stability of the system.  For example, if we never flushed MemRowSets, we would
eventually run out of memory.  As memory gets low, admissions control will slow
the pace of new requests getting accepted.


Decision Criteria
===============================================================================
The most important things that we need to weigh in order to make good decisions
are:
1. memory usage
2. tablet statistics
3. the age of memrowsets

Some other criteria that we considered, but rejected for v1 include:
1. free disk space.
2. load-balancing between disks or disksets which will be touched by
   maintenance operations

Free disk space should not be an issue in most competently administered setups.
We may revisit this later, but for the initial version, it is best to assume we
have enough space.

We can't consider disk-based scheduling right now since we don't have support
for multiple disks yet.


Memory usage
-------------------------------------------------------------------------------
Memory usage can be broken down into a few buckets:
1. System overhead (C++ data structures, operating system overheads, and so
   forth).
2. MemRowSets
3. The LRU block cache

We assume that #1 is relatively constant.  The maintenance op scheduler can
make tradeoffs between #2 and #3 by deciding to flush certain MemRowSets to
disk.

We want to keep the total amount of memory held by #1, #2 and #3 from growing
too large.  For now, our goal is to keep this sum relatively constant.  We have
not yet implemented giving memory held by tcmalloc back to the operating system.


Tablet Statistics
-------------------------------------------------------------------------------
If we know that a tablet's workload is scan-heavy (rather than insert-heavy),
we may wish to do a major delta compaction for that tablet to speed up scans.
It's probably smarter to do compactions on tables that are heavily used, than
on obscure tables that don't see much traffic.

This is probably the most difficult information source to make effective use
of, simply because it involves many workload-dependent assumptions and
heuristics.


The Age of MemRowSet objects
-------------------------------------------------------------------------------
MemRowSet and DeltaMemRowSet objects must be flushed to disk when they get too
old.  If we don't do this, the write-ahead log (WAL) will grow without bound.
This growth would waste disk space and slow startup to a crawl, since the
entire WAL must be traversed during the startup process.

We should embed a WAL op id in each MemRowSets and DeltaMemRowSet.  The
scheduler will look more favorably on the flushing of a MemRowSet as it ages.
After the operation id falls too far behind, it will try to flush the MemRowSet
no matter what.


Maintenance Operation types
===============================================================================

Maintenance operations to reduce memory usage
----------------------------------------

These operations spend some I/O or CPU in order to free up memory usage. They
may also incur further performance costs after completion. These cannot be
delayed indefinitely, as RAM is a finite resource.


MemStore Flush
------------------------------
Cost:
- Sequential I/O now (writing the actual memstore contents to disk)
- Sequential I/O later (frequent small flushes will cost more compactions down the road)

Benefit:
- RAM: frees up memory

Other/wash:
- At first glance, flushing might seem to increase cost of further insert/updates
  because it adds a new RowSet. However, because memstores are not compressed in
  any way, typically the newly flushed RowSet will be much smaller on disk than the
  memstore that it came from. This means that, even if we have to cache the whole
  result RowSet in the block cache, we're making much more effective use of RAM and
  thus may _reduce_ the total number of actual I/Os.


DeltaMemStore Flush
------------------------------
Basically the same costs as MemStore flush

Additional benefits:
TODO: flushing may also speed up scans substantially. Need to run experiments on this --
how much better is scanning a static cached file compared to scanning the equivalent
memstore. Maybe an order of magnitude.


LRU cache eviction
------------------------------
Cost: slower reads, slower inserts if evicting key columns or blooms
Benefit: frees RAM




Maintenance operations to manage future performance
----------------------------------------

These operations expend some kind of I/O and CPU now in order to improve the performance
of the system after they complete. They are only ever "necessary" in that if we put them
off forever, the system will slow to a crawl eventually.


Merging Compaction
------------------------------
Cost:
- Sequential I/O now (reading input, re-writing output)

Benefit:
- reduce the number of RowSets: speeds up inserts, updates. Speeds up short scans where blooms don't apply.


Minor Delta Compaction
------------------------------
Cost:
- Sequential I/O (reading input, re-writing output)

Benefit:
- Speeds up scans -- fewer delta trackers to apply
- May save disk space (eg when snapshot isolation is implemented, old version updates may be discarded)


Major delta compaction
------------------------------
Cost:
- Sequential I/O (reading input, re-writing output)

Benefit:
- Speeds up scans -- fewer delta trackers to apply, fewer total rows with deltas to apply.
- Save disk space (eg when snapshot isolation is implemented, old version updates may be discarded)

Relevant metrics:
- for each column, % of rows in RowSet which have been updated
- for each column, % of deltas which could be fully merged
- workload: scan heavy vs insert/update heavy?


Implementation Considerations
===============================================================================
Each tablet creates several MaintenanceOp objects, representing the various
maintenance operations which can be performed on it.  It registers these
operations with the MaintenanceManager.

The MaintenanceManager has a main thread which periodically polls the
registered MaintenanceOp objects and determines whether it should execute any
of them.  The default polling interval is 250 ms, but this is configurable.
Access to the MaintenanceOp is assumed to be thread-safe.  It's important to
note that the scheduler can choose any op available to it.  It is not bound to
execute operations on a first-come, first-serve basis.

If the MaintenanceManager decides to execute one of these operations, it will
run it in a thread-pool of configurable size.  We assume that maintenance
operations are blocking and require a thread context.  If the operation fails,
the MaintenanceManager will log a warning message and re-trigger the main
thread.  The failed MaintenanceOp will not be retried until a configurable
grace period has expired.

The MaintenanceOp has various fields indicating how much memory it will
probably free, how much CPU it will use, and so forth.  It also has a field
which marks it as not currently executable.  For example, this may be used by
some Ops that don't want multiple instances of themselves to run concurrently.

We want to keep at least one thread free to run flush operations, so that we
don't ever get into a situation where we need to free up memory, but all the
maintenance op threads are working on compactions or other operations.
Hopefully, most compactions will be reasonably short, so that we won't have to
schedule long compactions differently than short ones.
