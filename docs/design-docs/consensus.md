<!---
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

Kudu Consensus Design
============================================================

Note: This document is out of date. It should be updated to reflect design
decisions made and implementation done relating to Raft consensus [1].

This document introduces how Kudu will handle log replication and consistency
using an algorithm known as Viewstamped Replication (VS) and a series of
practical algorithms/techniques for recovery, reconfiguration, compactions etc.
This document introduces all the concepts directly related to Kudu, for any
missing information please refer to the original papers [1,3,4].

Quorums, in Kudu, are a set of collaborating processes that serve the purpose
of keeping a consistent, replicated log of operations on a given data set, e.g.
a tablet. This replicated consistent log, also plays the role of the Write
Ahead Log (WAL) for the tablet. Throughout this document we use config
participant and process interchangeably, these do not represent machines or OS
processes, as machines and or application daemons will participate in multiple
configs.

The write ahead log (WAL)
------------------------------------------------------------

The WAL provides strict ordering and durability guarantees:

1. If calls to Reserve() are externally synchronized, the order in
   which entries had been reserved will be the order in which they will
   be committed to disk.

2. If fsync is enabled (via the 'log_force_fsync_all' flag -- see
   log_util.cc; note: this is _DISABLED_ by default), then every single
   transaction is guaranteed to be synchronized to disk before its
   execution is deemed successful.

Log uses group commit to increase performance primarily by allowing
throughput to scale with the number of writer threads while
maintaining close to constant latency.

### Basic WAL usage

To add operations to the log, the caller must obtain the lock, and
call Reserve() with a collection of operations and pointer to the
reserved entry (the latter being an out parameter). Then, the caller
may release the lock and call the AsyncAppend() method with the
reserved entry and a callback that will be invoked upon completion of
the append. AsyncAppend method performs serialization and copying
outside of the lock.

For sample usage see mt-log-test.cc.

### Group commit implementation details

Currently, the group implementation uses a blocking queue (see
Log::entry_queue_ in log.h) and a separate long-running thread (see
Log::AppendThread in log.cc). Since access to the queue is
synchronized via a lock and only a single thread removes the queue,
the order in which the elements are added to the queue will be the
same as the order in which the elements are removed from the queue.

The size of the queue is currently based on the number of entries, but
this will eventually be changed to be based on size of all queued
entries in bytes.

#### Reserving a slot for the entry

Currently Reserve() allocates memory for a new entry on the heap each
time, marks the entry internally as "reserved" via a state enum, and
adds it to the above-mentioned queue. In the future, a ring-buffer or
another similar data structure could be used that would take the place
of the queue and make allocation unnecessary.

#### Copying the entry contents to the reserved slot

AsyncAppend() serializes the contents of the entry to a buffer field
in the entry object (currently the buffer is allocated at the same
time as the entry itself); this avoids contention that would occur if
a shared buffer was to be used.

#### Synchronizing the entry contents to disk

A separate appender thread waits until entries are added to the
queue. Once the queue is no longer empty, the thread grabs all
elements on the queue. Then for each dequeued entry, the appender
waits until the entry is marked ready (see "Copying the entry contents
to the reserved slot" above) and then appends the entry to the current
log segment without synchronizing the underlying file with filesystem
(env::WritableFile::Append())

Note: this could be further optimized by calling AppendVector() with a
vector of buffers from all of the consumed entries.

Once all entries are successfully appended, the appender thread syncs
the file to disk (env::WritableFile::Sync()) and (again) waits until
more entries are added to the queue, or until the queue or the
appender thread are shut down.

### Log segment files and asynchronous preallocation

Log uses PosixWritableFile() for underlying storage. If preallocation
is enabled ('--log_preallocate_segments' flag, defined in log_util.cc,
true by default), then whenever a new segment is created, the
underlying file is preallocated to a certain size in megabytes
('--log_segment_size_mb', defined in log_util.cc, default 64). While
the offset in the segment file is below the preallocated length,
the cheaper fdatasync() operation is used instead of fsync().

When the size of the current segment exceeds the preallocated size, a
task is launched in a separate thread that begins preallocating the
underlying file for the new log segment; meanwhile, until the task
finishes, appends still go to the existing file.  Once the new file is
preallocated, it is renamed to the correct name for the next segment
and is swapped in place of the current segment.

When the current segment is closed without reaching the preallocated
size, the underlying file is truncated to the last written offset
(i.e., the actual size).

Configs and roles within configs
------------------------------------------------------------

A config in Kudu is a fault-tolerant, consistent unit that serves requests for
a single tablet. As long as there are 2f+1 participants available in a config,
where f is the number of possibly faulty participants, the config will keep
serving requests for its tablet and it is guaranteed that clients perceive a
fully consistent, linearizable view of both data and operations on that data.
The f parameter, defined table wide through configuration implicitly
defines the size of the config, f=0 indicates a single node config, f=1
indicates a 3 node config, f=2 indicates a 5 node config, etc.. Quorums may
overlap in the sense that each physical machine may be participating in
multiple configs, usually one per each tablet that it serves.

Within a single config, in steady state, i.e. when no peer is faulty, there
are two main types of peers. The leader peer and the follower peers.
The leader peer dictates the serialization of the operations throughout the
config, its version of the sequence of data altering requests is the "truth"
and any data altering request is only considered final (i.e. can be
acknowledged to the client as successful) when a majority of the config
acknowledges that they "agree" with the leader's view of the event order.
In practice this means that all write requests are sent directly to the
leader, which then replicates them to a majority of the followers before
sending an ACK to the client. Follower peers are completely passive in
steady state, only receiving data from the leader and acknowledging back.
Follower peers only become active when the leader process stops and one
of the followers (if there are any) must be elected leader.

Participants in a config may be assigned the following roles:

LEADER - The current leader of the config, receives requests from clients
and serializes them to other nodes.

FOLLOWER - Active participants in the config, whose votes count towards
majority, replication count etc.

LEARNER - Passive participants in the config, whose votes do not count
towards majority or replication count. New nodes joining the config
will have this role until they catch up and can be promoted to FOLLOWER.

NON_PARTICIPANT - A peer that does not participate in a particular
config. Mostly used to mark prior participants that stopped being so
on a configuration change.

The following diagram illustrates the possible state changes:

```
                 +------------+
                 |  NON_PART  +---+
                 +-----+------+   |
   Exist. RaftConfig?  |          |
                 +-----v------+   |
                 |  LEARNER   +   | New RaftConfig?
                 +-----+------+   |
                       |          |
                 +-----v------+   |
             +-->+  FOLLOW.   +<--+
             |   +-----+------+
             |         |
             |   +-----v------+
  Step Down  +<--+ CANDIDATE  |
             ^   +-----+------+
             |         |
             |   +-----v------+
             +<--+   LEADER   |
                 +------------+
```

Additionally all states can transition to NON_PARTICIPANT, on configuration
changes and/or peer timeout/death.

Assembling/Rebooting a RaftConfig and RaftConfig States
------------------------------------------------------------

Prior to starting/rebooting a peer, the state in WAL must have been replayed
in a bootstrap phase. This process will yield an up-to-date Log and Tablet.
The new/rebooting peer is then Init()'ed with this Log. The Log is queried
for the last committed configuration entry (A Raft configuration consists of
a set of peers (uuid and last known address) and hinted* roles). If there is
none, it means this is a new config.

After the peer has been Init()'ed, Start(Configuration) is called. The provided
configuration is a hint which is only taken into account if there was no previous
configuration*.

Independently of whether the configuration is a new one (new config)
or an old one (rebooting config), the config cannot start until a
leader has been elected and replicates the configuration through
consensus. This ensures that a majority of nodes agree that this is
the most recent configuration.

The provided configuration will always specify a leader -- in the case
of a new config, it is chosen by the master, and in the case of a
rebooted one, it is the configuration that was active before the node
crashed. In either case, replicating this initial configuration
entry happens in the exact same way as any other config entry,
i.e. the LEADER will try and replicate it to FOLLOWERS. As usual if
the LEADER fails, leader election is triggered and the new LEADER will
try to replicate a new configuration.

Only after the config has successfully replicated the initial configuration
entry is the config ready to accept writes.


Peers in the config can therefore be in the following states:

BOOTSTRAPPING - The phase prior to initialization where the Log is being
replayed. If a majority of peers are still BOOTSTRAPPING, the config doesn't
exist yet.

CONFIGURING: Until the current configuration is pushed though consensus. This
is true for both new configs and rebooting configs. The peers do not accept
client requests in this state. In this state, the Leader tries to replicate
the configuration. Followers run failure detection and trigger leader election
if the hinted leader doesn't successfully replicate within the configured
timeout period.

RUNNING: The LEADER peer accepts writes and replicates them through consensus.
FOLLOWER replicas accepts writes from the leader and ACK.

* The configuration provided on Start() can only be taken into account if there
is an appropriate leader election algorithm. This can be added later but is not
present in the initial implementation. Roles are hinted in the sense that the
config initiator (usually the master) might hint what the roles for the peers
in the config should be, but the config is the ultimate decider on whether that
is possible or not.

References
------------------------------------------------------------

1. [Raft: In Search of an Understandable Consensus Algorithm]
   (https://www.usenix.org/system/files/conference/atc14/atc14-paper-ongaro.pdf).
   D. Ongaro and J. Ousterhout.
   2014 USENIX Annual Technical Conference (USENIX ATC 14). 2014.
2. [ARIES: A transaction recovery method supporting fine-granularity locking and partial
   rollbacks using write-ahead logging](http://www.cs.berkeley.edu/~brewer/cs262/Aries.pdf).
   C. Mohan, D. Haderle, D. Lindsay, H. Pirahesh, and P. Schwartz.
   ACM Transactions on Database Systems (TODS) 17.1 (1992): 94-162.
3. [Viewstamped replication: A new primary copy method to support highly-available
   distributed systems](http://www.pmg.csail.mit.edu/papers/vr.pdf).
   B. Oki and B. Liskov.
   Proceedings of the seventh annual ACM Symposium on Principles of distributed computing.
   ACM, 1988.
4. [Viewstamped Replication Revisited](http://pmg.csail.mit.edu/papers/vr-revisited.pdf).
   B. Liskov and J. Cowling. 2012.
5. [Aether: A Scalable Approach to logging](http://pandis.net/resources/vldb10aether.pdf).
   R. Johnson, I. Pandis, R. Stoica, M. Athanassoulis, and A. Ailamaki.
   Proceedings of the VLDB Endowment 3.1-2 (2010): 681-692.
