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
#ifndef KUDU_MASTER_TS_DESCRIPTOR_H
#define KUDU_MASTER_TS_DESCRIPTOR_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>
#include <glog/logging.h>
#include <gtest/gtest_prod.h>

#include "kudu/common/wire_protocol.pb.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/macros.h"
#include "kudu/util/locks.h"
#include "kudu/util/make_shared.h"
#include "kudu/util/monotime.h"
#include "kudu/util/status.h"

namespace kudu {

class Sockaddr;

namespace consensus {
class ConsensusServiceProxy;
}

namespace rpc {
class Messenger;
}

namespace tserver {
class TabletServerAdminServiceProxy;
}

namespace master {

// Master-side view of a single tablet server.
//
// Tracks the last heartbeat, status, instance identifier, location, etc.
// This class is thread-safe.
class TSDescriptor : public enable_make_shared<TSDescriptor> {
 public:
  static Status RegisterNew(const NodeInstancePB& instance,
                            const ServerRegistrationPB& registration,
                            std::shared_ptr<TSDescriptor>* desc);

  virtual ~TSDescriptor() = default;

  // Set the last-heartbeat time to now.
  void UpdateHeartbeatTime();

  // Return the amount of time since the last heartbeat received
  // from this TS.
  MonoDelta TimeSinceHeartbeat() const;

  // Return whether this server is presumed dead based on last heartbeat time.
  bool PresumedDead() const;

  // Register this tablet server.
  Status Register(const NodeInstancePB& instance,
                  const ServerRegistrationPB& registration);

  const std::string &permanent_uuid() const { return permanent_uuid_; }
  int64_t latest_seqno() const;

  // Copy the current registration info into the given PB object.
  // A safe copy is returned because the internal Registration object
  // may be mutated at any point if the tablet server re-registers.
  void GetRegistration(ServerRegistrationPB* reg) const;

  void GetNodeInstancePB(NodeInstancePB* instance_pb) const;

  // Return an RPC proxy to the tablet server admin service.
  Status GetTSAdminProxy(const std::shared_ptr<rpc::Messenger>& messenger,
                         std::shared_ptr<tserver::TabletServerAdminServiceProxy>* proxy);

  // Return an RPC proxy to the consensus service.
  Status GetConsensusProxy(const std::shared_ptr<rpc::Messenger>& messenger,
                           std::shared_ptr<consensus::ConsensusServiceProxy>* proxy);

  // Increment the accounting of the number of replicas recently created on this
  // server. This value will automatically decay over time.
  void IncrementRecentReplicaCreations();

  // Return the number of replicas which have recently been created on this
  // TS. This number is incremented when replicas are placed on the TS, and
  // then decayed over time. This method is not 'const' because each call
  // actually performs the time-based decay.
  double RecentReplicaCreations();

  // Set the number of live replicas (i.e. running or bootstrapping).
  void set_num_live_replicas(int n) {
    DCHECK_GE(n, 0);
    std::lock_guard<simple_spinlock> l(lock_);
    num_live_replicas_ = n;
  }

  // Return the number of live replicas (i.e running or bootstrapping).
  int num_live_replicas() const {
    std::lock_guard<simple_spinlock> l(lock_);
    return num_live_replicas_;
  }

  // Return the location of the tablet server. This returns a safe copy
  // since the location could change at any time if the tablet server
  // re-registers.
  boost::optional<std::string> location() const {
    std::lock_guard<simple_spinlock> l(lock_);
    return location_;
  }

  // Return a string form of this TS, suitable for printing.
  // Includes the UUID as well as last known host/port.
  std::string ToString() const;

 protected:
  explicit TSDescriptor(std::string perm_id);

 private:
  FRIEND_TEST(TestTSDescriptor, TestReplicaCreationsDecay);
  friend class PlacementPolicyTest;

  Status RegisterUnlocked(const NodeInstancePB& instance,
                          const ServerRegistrationPB& registration);

  // Uses DNS to resolve registered hosts to a single Sockaddr.
  // Returns the resolved address as well as the hostname associated with it
  // in 'addr' and 'host'.
  Status ResolveSockaddr(Sockaddr* addr, std::string* host) const;

  void DecayRecentReplicaCreationsUnlocked();

  mutable simple_spinlock lock_;

  const std::string permanent_uuid_;
  int64_t latest_seqno_;

  // The last time a heartbeat was received for this node.
  MonoTime last_heartbeat_;

  // The number of times this tablet server has recently been selected to create a
  // tablet replica. This value decays back to 0 over time.
  double recent_replica_creations_;
  MonoTime last_replica_creations_decay_;

  // The number of live replicas on this host, from the last heartbeat.
  int num_live_replicas_;

  // The tablet server's location, as determined by the master at registration.
  boost::optional<std::string> location_;

  gscoped_ptr<ServerRegistrationPB> registration_;

  std::shared_ptr<tserver::TabletServerAdminServiceProxy> ts_admin_proxy_;
  std::shared_ptr<consensus::ConsensusServiceProxy> consensus_proxy_;

  DISALLOW_COPY_AND_ASSIGN(TSDescriptor);
};

// Alias for a vector of tablet server descriptors.
typedef std::vector<std::shared_ptr<TSDescriptor>> TSDescriptorVector;

} // namespace master
} // namespace kudu
#endif /* KUDU_MASTER_TS_DESCRIPTOR_H */
