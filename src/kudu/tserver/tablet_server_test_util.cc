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

#include "kudu/tserver/tablet_server_test_util.h"

#include "kudu/consensus/consensus.proxy.h"
#include "kudu/server/server_base.proxy.h"
#include "kudu/tserver/tablet_copy.proxy.h"
#include "kudu/tserver/tserver_admin.proxy.h"
#include "kudu/tserver/tserver_service.proxy.h"
#include "kudu/util/net/sockaddr.h"

namespace kudu {
namespace tserver {

using consensus::ConsensusServiceProxy;
using rpc::Messenger;
using std::shared_ptr;

void CreateTsClientProxies(const Sockaddr& addr,
                           const shared_ptr<Messenger>& messenger,
                           std::unique_ptr<TabletCopyServiceProxy>* tablet_copy_proxy,
                           std::unique_ptr<TabletServerServiceProxy>* tablet_server_proxy,
                           std::unique_ptr<TabletServerAdminServiceProxy>* admin_proxy,
                           std::unique_ptr<ConsensusServiceProxy>* consensus_proxy,
                           std::unique_ptr<server::GenericServiceProxy>* generic_proxy) {
  const auto& host = addr.host();
  tablet_copy_proxy->reset(new TabletCopyServiceProxy(messenger, addr, host));
  tablet_server_proxy->reset(new TabletServerServiceProxy(messenger, addr, host));
  admin_proxy->reset(new TabletServerAdminServiceProxy(messenger, addr, host));
  consensus_proxy->reset(new ConsensusServiceProxy(messenger, addr, host));
  generic_proxy->reset(new server::GenericServiceProxy(messenger, addr, host));
}

} // namespace tserver
} // namespace kudu
