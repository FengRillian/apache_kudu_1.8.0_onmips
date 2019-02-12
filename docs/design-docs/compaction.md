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

Compaction design notes
=======================

This document explains the mechanics of performing a rowset flush/compaction.
For details explaining how compactions are selected, see compaction-policy.md.
NOTE: this does not describe anything about flushing delta stores to delta files!

Goal: Take two or more RowSets with overlapping key ranges, and merge
them into a new RowSet, while updates are concurrently being applied.
The output RowSet should also garbage collect (i.e reclaim storage from)
any rows which were deleted in the old RowSets.

Let's start with the simple example of compacting from 1 input rowset to
1 output rowset. This has the effect of removing GC-able data and
applying updates. The compaction has two main phases:

```
      "flush_snap"
           |
           |
  before   v
<----------|
              Phase 1:
          merging/flushing
           |-----------|
                         Phase 2: migrate
                         deltas
                       |---------------|
                                         compaction
                                         complete
                                       |----------->

|--------------  time ----------------------------->
```

System steady state:
  - Updates are applied only to the "source RowSet"

Transition into Phase 1:
  - Create a snapshot iterator to merge the input RowSets, and save the
    associated MVCC snapshot state.

Phase 1: merge/flush data:
  - Use the iterator created above to create a new set of data for the output
    RowSet. This will reflect any updates or deletes which arrived prior to the
    start of phase 1, but no updates or deletes which arrive during either
    phase of the compaction.

  - Any mutations which arrive during this phase are applied only to the input
    RowSets' delta tracking structures. Because the merge operates on a snapshot,
    it will not take these into account in the output RowSet.

Phase 2: migrate deltas from phase 1
  - Any mutations which arrive during this phase should be applied to both the
    input RowSet and the output RowSet. This is simple to do by duplicating
    the key lookup into the output RowSet's key column when the update arrives.
    This is implemented by swapping in a "DuplicatingRowSet" implementation which
    forwards updates to both the input and output rowsets.

  - Any reads during this phase must be served from the input RowSet, since the
    output RowSet is missing the deltas which arrived during the merge phase.

  - Because the merge output ignored any mutations which arrived during phase 1,
    we must now 'migrate' those mutations to the output RowSet. This can be done
    efficiently by collecting all of the deltas which were not included in the
    snapshot iterator, and applying them to the output rowset's delta tracker.


End of Phase 2: swap RowSets
  - After Phase 2, the two RowSets have logically identical data, and they may
    be atomically swapped. Once the output RowSet has been swapped in, new updates
    only need to be applied to the output RowSet, and the old RowSet may be dropped.

Extending to multiple RowSets
-----------------------------

The above algorithm can be extended to multiple RowSets equally well. At the beginning
of the compaction, each RowSet is snapshotted, and a snapshot iterator created. A merge
iterator then performs the merge of all of the snapshots in ascending key order.

