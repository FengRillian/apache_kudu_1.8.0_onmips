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

Compaction Policy
============================================================

This document explains the policy of performing a compaction.
For details explaining how compactions are implemented, see compaction.md.

The compaction policy is responsible for selecting a set of rowsets to compact
together. Compactions are necessary in order to reduce the number of DiskRowSets
which must be consulted for various operations, thus improving the overall
performance of the tablet.

Coming up with a good compaction policy is a balancing act between several goals:

1. Re-arrange the physical layout to be more efficient for subsequent operations.

2. Do so without using too many resources in the compaction itself.

3. Do so "smoothly" - spread work out over time so that operation performance is
   predictable and reasonably constant.


The following sections provide some analysis of the above goals:

Benefit of compaction for subsequent operations
============================================================

In order to determine a good compaction policy, we want to define a cost measure
for a given set of RowSets within a tablet. Consider the following set of
RowSets:

```
   1     2      3     4    5
|--A--||-B--||--C--||---D----|
|--------------E-------------|
                   |-F--|
```

In this diagram, the key space spans from left to right, and each RowSet is drawn
as an interval based on its first and last contained key. We'll define a few terms
for later use in this document:

**Width**

Let the Width of a RowSet be proportional to the percentage of key
space that it spans. For example, rowset E has a width of 1, since
it spans the whole tablet. Rowset B has width 0.2, since it spans
about 20% of the tablet.

Note that the Width is also the probability that any read in a
uniform random read workload will have to consult that RowSet.

**Height**

The "Height" of a tablet at a given key is the number of rowsets
whose key ranges contain that key. For example, the height of the
above tablet at key 1 is 2, since rowsets A and E span that key.
The height at key 4 is 3, since D, E, and F span that key.

The Height at any key is the number of RowSets that will be have to
be consulted for a random read of that key.

Let us consider the cost of various operations on the tablet:

Insert
-------
In order to Insert, each of the rowsets must be checked for a duplicate key. By
storing the rowset ranges in an interval tree, we can efficiently determine the
set of rowsets whose intervals may contain the key to be inserted, and thus the
cost is linear in that number of rowsets:

```
Let n = the Height of the tablet at the given key
Let B = the bloom filter false positive rate
Let C_bf = cost of bloom filter check
Let C_pk = cost of a primary key lookup
Cost = n*C_bf + n*B*C_pk
Cost = n(C_bf + B*C_pk)
```

Typically, B is approximately 1% or lower, so the bloom filter checks dominate this
equation. However, in some cases where the primary key column is very large, every
primary key check will incur a disk seek, meaning that `C_pk` is orders of magnitude
higher than `C_bf` (which we expect to be in RAM or SSD). So, we cannot fully ignore
the term resulting from the bloom filter misses.

Random read
------------
The costs for random read are similar to the cost for inserts: given the known key,
each potentially overlapping rowset must be queried.


Short Scan
-----------
Scans cannot make use of bloom filters, so the cost is similar to the above, except
that all overlapping rowsets must be seeked by PK:

```
Cost = n*C_pk
```

We assume a "short" scan is one in which the sequential IO cost after finding the start
key is small compared to the seek cost. (eg assuming a 10ms seek time, 1MB or less of
sequential IO).


Long scan (e.g. full table scan):
---------------------------------
A long scan is likely to retrieve data from many rowsets. In this case, the size
of the rowsets comes into play.

Let S = the number of MB in the scan
Let B = the disk bandwidth (MB/sec)
Let n = the number of rowsets accessed, as before

Assume that accessing each rowset costs 1 seek (same as `C_pk`).

```
Cost = n*C_pk + S/B
```

To summarize the above, all of the costs of operations are heavily dependent on the
number of rowsets which must be accessed. Therefore, to minimize cost, we should
follow the following strategies:

1. In the case of point queries (inserts and random read/short scan), merge
   rowsets which overlap in keyspace, thus reducing the average height of the
   Tablet.

2. In the case of longer scans, merge together rowsets to improve the ratio of
   sequential IO to seeks.

We can assume that, so long as the rowsets are reasonably large, goal #2 above has
diminishing returns after rowsets achieve ~10MB or so of sequential IO for every
seek (1 seek ~= 10ms, 10MB IO ~= 100ms). However, goal #1 has linear returns, so we
focus on goal #1.


Cost of doing a compaction
============================================================
According to the above analysis, the optimal configuration for a tablet is a
single giant rowset which spans the entirety of the key space. This is
intuitively true: a fully-compacted tablet is going to perform the best because
every access will require at most one bloom filter check and one seek.

However, it is obviously not optimal to simply compact all RowSets together in every
compaction. This would be inefficient, since every compaction would rewrite the
entire rowset, causing huge write amplification and wasted IO for only a small
amount of efficiency gain.

So, we need to consider not just how efficient the resulting tablet would be, but also
how expensive it is to perform the candidate compaction. Only by weighing those two
against each other can we decide on the best compaction to perform at any given point
in time.

For the purposes of this analysis, we consider the cost of a compaction to simply be
the sum of the IO performed by the compaction. We'll assume that deletions are rare,
in which case the output data size of a compaction is approximately equal to the
input data size. We also assume that the compaction inputs are large enough that
sequential IO outweighs any seeks required.

Thus the cost of performing a compaction is O(input size).


Incremental work
============================================================
The third goal for compaction is to be able to perform work incrementally. Doing
frequent incremental compactions rather than occasional large ones results in a
more consistent performance profile for end-user applications. Incremental work
also allows the system to react more quickly to changes in workload: for example,
if one area of the keyspace becomes hot, we would like to be able to quickly
react and compact that area of the keyspace within a short time window.

One way to achieve this goal is to put a bound on the amount of data that any
given compaction will read and write. Bounding this data on the range of several
hundred MB means that a compaction can occur in 10 seconds or less, allowing
quick reaction time to shifts in workload.


Proposed strategy:
============================================================

Limiting RowSet Sizes
------------------------------
The first key piece of the proposed compaction strategy is to limit the maximum size of
any RowSet to a relatively small footprint - e.g 64MB or even less. This can be done
by modifying the DiskRowSet writer code to "roll over" to a new rowset after the size
threshold has been reached. Thus, even if flushing a larger dataset from memory, the
on-disk rowset sizes can be limited.


Flushes with limited RowSet size
---------------------------------
For example, imagine that the max rowset size is set to 64MB, and 150MB of data has
accumulated in the MemRowSet before a flush. The resulting output of the flush, then
looks like:

```
   A       B     C
|------||------||--|
  64MB    64MB  22MB
```

Note that even though the maximum DiskRowSet size is 64MB, the third flushed rowset
will be smaller. In the future, we could esimate the on-disk data size and try to make
the three RowSets approximately equal-sized, but it is not necessary for correctness.

Compactions with limited RowSet size
-------------------------------------
Now imagine another scenario, where a Tablet flushes several times, each resulting in
small files which span the entirety of the key space -- commonly seen in a uniform
random insert load. After 3 flushes, the Tablet looks like:

```
       A (50MB)
|-------------------|
       B (50MB)
|-------------------|
       C (50MB)
|-------------------|
```

Because the three rowset ranges overlap, every access to the tablet must query each of the
rowsets (i.e the average rowset "depth" is 3). If the compaction policy selects these
three RowSets for compaction, the compaction result will look like:

```
   D       E     F
|------||------||--|
  64MB    64MB  22MB
```

Essentially, the compaction reorganizes the data from overlapping rowsets into non-overlapping
rowsets of a similar size. This reduces the average depth from 3 to 1, improving the
Tablet performance.


Dealing with large numbers of RowSets
--------------------------------------
With these limited sizes, a modestly sized Tablet (eg 20GB) will have on the order of hundreds
of RowSets. In order to efficiently determine the set of RowSets which may contain a given
query key or range, we have to change the Tablet code to store the RowSets in an interval
tree instead of a simple list. The Interval Tree is a data structure which provides efficient
query for the set of intervals overlapping a given query point or query interval.


Intuition behind compaction selection policy
---------------------------------------------
As a simplification, assume for now that all RowSets are exactly the same size (rather
than bounded under a maximum). Then, we can classify a RowSet as "good" or "bad" based on
one simple factor: the smaller the range of key space that it spans, the better.
Assuming a uniform insert workload, every flushed RowSet will span the entirety of the
Tablet's key space -- and hence must be queried by every subsequent operation. Once there
are multiple such flushed RowSets (A, B, and C in the diagram), compacting them results in
skinnier rowsets D, E, and F.

Intuitively, then, a good compaction policy finds rowsets which are wide and overlapping, and
compacts them together, resulting in rowsets which are skinny and non-overlapping.

Taking the cost factors developed above, we can look at compaction selection as an optimization
problem: reduce the cost of the Tablet configuration as much as possible under a given IO budget.

Per the analysis above, the cost of a single read or insert is linear in the "height" of the
RowSets at the key being accessed. So, the average cost of operations can be calculated by
integrating the tablet height across the key space, or equivalently adding up the widths
of all of the RowSets. For example:

```
          |---A----| (width 10)
     |-----B-------| (width 15)
|-C-||-----D-------| (width 5, width 15)
|--------E---------| (width 20)
```

So, the summed width = 20+5+15+15+10 = 65.

Imagine that we choose to compact rowsets A, B, and D above, resulting in the following
output:

```
|-C-||-F-||-G-||-H-| (width 5, width 5, width 5, width 5)
|--------E---------| (width 20)
```

Note that the total number of bytes have not changed: we've just reorganized the bytes
into a more compact form, reducing the average height of the tablet.

Now the summed cost is 40. So, the compaction had benefit 25, using a budget of 3 units of IO
(remember that rowsets are assumed to be constant size for this analysis).

Another choice for the compaction might have been to compact B, D, and E, resulting in:

```
          |---A----| (width 10)
|-C-|                (width 5)
|---F--||--G--||-H-| (width 8, width 7, width 5)
```

This compaction reduced the tablet cost from 65 to 35 -- so its benefit was 30, using the same
IO budget of 3.

Given that the second compaction choice reduced the tablet height more using the same budget,
it is a more optimal solution.

Mathematical analysis
-----------------------
The reduction of cost due to a compaction is simple to calculate:

Cost change = sum(original rowset widths) - sum(output rowset widths)

We know that the output rowsets will not overlap at all, and that their total width will
span the union of the input rowset ranges. Therefore:

Cost change = sum(original rowset widths) - (union width of original rowsets)

Note that, for this analysis, the key ranges are treated as integers. This can be extended
to string keys in a straightforward manner by treating the string data as unsigned integers.

Algorithm
----------

Given budget N rowsets:

```
For each pair of rowsets (A, B):
  Evaluate BestForPair(A, B):

BestForPair(A, B):
  Let union width = max(A.max_key, B.max_key) - min(A.min_key, B.min_key)
  Determine the subset R of rowsets that are fully contained within the range A, B
  Evaluate PickRowsetsWithBudget(R, N):
  Set objective = sum(rowset width) - union width
  If objective > best objective:
    best solution = this set

PickRowsetsWithBudget(R, N):
  Choose the N rowsets in R which which maximize sum(rowset width)
```


PickRowsetsWithBudget can be solved by simply sorting the rowsets by their width and
choosing the top N.


Extending algorithm to non-constant sizes
------------------------------------------

Even though we limit the maximum rowset size to a constant, some rowsets may be smaller
due to more frequent flushes, etc. Thus, we would like to change the budget to be a number
of MB of IO, rather than a simple count N of input files. The subproblem PickNRowSets then becomes:

> Choose a set of RowSets such that their total file size falls within a budget, and
> maximizes their total widths.

This is an instance of the 0-1 knapsack problem, so we replace PickRowsetsWithBudget(R, N)
with a knapsack problem solver.

Computational complexity
----------------------------

The algorithm contains `O(n^2)` calls to BestForPair, each of which contains one instance of the
0-1 knapsack problem, which has complexity `O(n * max_budget)`. Thus, the total complexity is cubic
in the number of rowsets, which can become quite expensive when a given tablet may include on the
order of a thousand rowsets.

We can optimize the approach by changing the order in which we consider pairs (A, B) in the
above-described algorithm:

```
For each rowset A:
  candidates = all rowsets B such that B.min_key >= A.min_key
  sort candidates B by increasing B.max
  For each pair (A, B):
    Evaluate BestForPair(A, B)
```

Considering the pairs in this order simplifies BestForPair as follows:

```
BestForPair(A, B):
  Let union width = max(A.max_key, b.max_key) - min(A.min_key, B.min_key)
  Determine the subset R of rowsets that are fully contained within the range A, B
   ** Because B.max_key is non_decreasing, this subset R is identical to R in the
      previous call, except that B is now added to the end. No extra loop
      is required.
  Evaluate PickRowsetsWithBudget(R, N):
   ** This instantiation of the knapsack problem now is identical to the previous
      instantiation, except with one additional item. Thus, it can be computed
      incrementally from the previous solution.
  Set objective = sum(rowset width) - union width
  If objective > best objective:
    best solution = this set
```


Additionally, upper bounds can be calculated by solving the simpler fractional knapsack
problem and used to short-circuit the more complex calculations.


Extending algorithm to non-uniform workloads
--------------------------------------------

The above analysis is done in terms of constant workloads. However, in practice, workloads
may be skewed. Given that, it is more important to compact the areas of the key space which
are seeing frequent access. The algorithms can be extended in a straightforward way by changing
all references to the "width" of a rowset to instead be CDF(max key) - CDF(min key) where CDF
is the cumulative distribution function for accesses over a lagging time window.
