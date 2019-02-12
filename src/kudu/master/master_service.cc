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

#include "kudu/master/master_service.h"

#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <gflags/gflags.h>
#include <gflags/gflags_declare.h>
#include <glog/logging.h>

#include "kudu/common/common.pb.h"
#include "kudu/common/wire_protocol.h"
#include "kudu/common/wire_protocol.pb.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/consensus/replica_management.pb.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/hms/hms_catalog.h"
#include "kudu/master/catalog_manager.h"
#include "kudu/master/master.h"
#include "kudu/master/master.pb.h"
#include "kudu/master/master_cert_authority.h"
#include "kudu/master/ts_descriptor.h"
#include "kudu/master/ts_manager.h"
#include "kudu/rpc/remote_user.h"
#include "kudu/rpc/rpc_context.h"
#include "kudu/security/token.pb.h"
#include "kudu/security/token_signer.h"
#include "kudu/security/token_verifier.h"
#include "kudu/server/server_base.h"
#include "kudu/util/flag_tags.h"
#include "kudu/util/logging.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/sockaddr.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/status.h"

DECLARE_bool(hive_metastore_sasl_enabled);
DECLARE_bool(raft_prepare_replacement_before_eviction);
DECLARE_string(hive_metastore_uris);

DEFINE_int32(master_inject_latency_on_tablet_lookups_ms, 0,
             "Number of milliseconds that the master will sleep before responding to "
             "requests for tablet locations.");
TAG_FLAG(master_inject_latency_on_tablet_lookups_ms, unsafe);
TAG_FLAG(master_inject_latency_on_tablet_lookups_ms, hidden);

DEFINE_bool(master_support_connect_to_master_rpc, true,
            "Whether to support the ConnectToMaster() RPC. Used for testing "
            "version compatibility fallback in the client.");
TAG_FLAG(master_support_connect_to_master_rpc, unsafe);
TAG_FLAG(master_support_connect_to_master_rpc, hidden);

DEFINE_bool(master_non_leader_masters_propagate_tsk, false,
            "Whether a non-leader master sends information about its TSKs in "
            "response to a tablet server's heartbeat. This is intended for "
            "tests scenarios only and should not be used elsewhere.");
TAG_FLAG(master_non_leader_masters_propagate_tsk, hidden);

using google::protobuf::Message;
using kudu::consensus::ReplicaManagementInfoPB;
using kudu::pb_util::SecureDebugString;
using kudu::pb_util::SecureShortDebugString;
using kudu::security::SignedTokenPB;
using kudu::server::ServerBase;
using std::shared_ptr;
using std::string;
using std::vector;
using strings::Substitute;

namespace kudu {
namespace master {

namespace {

// If 's' is not OK and 'resp' has no application specific error set,
// set the error field of 'resp' to match 's' and set the code to
// UNKNOWN_ERROR.
template<class RespClass>
void CheckRespErrorOrSetUnknown(const Status& s, RespClass* resp) {
  if (PREDICT_FALSE(!s.ok() && !resp->has_error())) {
    StatusToPB(s, resp->mutable_error()->mutable_status());
    resp->mutable_error()->set_code(MasterErrorPB::UNKNOWN_ERROR);
  }
}

} // anonymous namespace

MasterServiceImpl::MasterServiceImpl(Master* server)
  : MasterServiceIf(server->metric_entity(), server->result_tracker()),
    server_(server) {
}

bool MasterServiceImpl::AuthorizeClient(const Message* /*req*/,
                                        Message* /*resp*/,
                                        rpc::RpcContext* context) {
  return server_->Authorize(context, ServerBase::SUPER_USER | ServerBase::USER);
}

bool MasterServiceImpl::AuthorizeServiceUser(const Message* /*req*/,
                                             Message* /*resp*/,
                                             rpc::RpcContext* context) {
  // We don't allow superusers to pretend to be tablet servers -- there are no
  // operator tools that do anything like this and since we sign requests for
  // tablet servers, we should be extra tight here.
  return server_->Authorize(context, ServerBase::SERVICE_USER);
}

bool MasterServiceImpl::AuthorizeClientOrServiceUser(const Message* /*req*/,
                                                     Message* /*resp*/,
                                                     rpc::RpcContext* context) {
  return server_->Authorize(context, ServerBase::SUPER_USER | ServerBase::USER |
                            ServerBase::SERVICE_USER);
}

bool MasterServiceImpl::AuthorizeSuperUser(const Message* /*req*/,
                                           Message* /*resp*/,
                                           rpc::RpcContext* context) {
  return server_->Authorize(context, ServerBase::SUPER_USER);
}

void MasterServiceImpl::Ping(const PingRequestPB* /*req*/,
                             PingResponsePB* /*resp*/,
                             rpc::RpcContext* rpc) {
  rpc->RespondSuccess();
}

void MasterServiceImpl::TSHeartbeat(const TSHeartbeatRequestPB* req,
                                    TSHeartbeatResponsePB* resp,
                                    rpc::RpcContext* rpc) {
  // If CatalogManager is not initialized don't even know whether
  // or not we will be a leader (so we can't tell whether or not we can
  // accept tablet reports).
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedOrRespond(resp, rpc)) {
    return;
  }
  bool is_leader_master = l.leader_status().ok();

  // 2. All responses contain this.
  resp->mutable_master_instance()->CopyFrom(server_->instance_pb());
  resp->set_leader_master(is_leader_master);

  // 3. Register or look up the tserver.
  shared_ptr<TSDescriptor> ts_desc;
  if (req->has_registration()) {
    const auto scheme = FLAGS_raft_prepare_replacement_before_eviction
        ? ReplicaManagementInfoPB::PREPARE_REPLACEMENT_BEFORE_EVICTION
        : ReplicaManagementInfoPB::EVICT_FIRST;
    const auto ts_scheme = req->has_replica_management_info()
        ? req->replica_management_info().replacement_scheme()
        : ReplicaManagementInfoPB::EVICT_FIRST;
    // If the catalog manager is running with some different replica management
    // scheme, report back an error and do not register the tablet server.
    if (scheme != ts_scheme) {
      const auto& ts_uuid = req->common().ts_instance().permanent_uuid();
      const auto& ts_addr = rpc->remote_address().ToString();
      Status s = Status::ConfigurationError(
          Substitute("replica replacement scheme ($0) of the tablet server "
                     "$1 at $2 differs from the catalog manager's ($3); "
                     "they must be run with the same scheme (controlled "
                     "by the --raft_prepare_replacement_before_eviction flag)",
                     ReplicaManagementInfoPB::ReplacementScheme_Name(ts_scheme),
                     ts_uuid, ts_addr,
                     ReplicaManagementInfoPB::ReplacementScheme_Name(scheme)));
      LOG(WARNING) << s.ToString();

      auto* error = resp->mutable_error();
      StatusToPB(s, error->mutable_status());
      error->set_code(MasterErrorPB::INCOMPATIBLE_REPLICA_MANAGEMENT);

      // Yes, this is confusing: the RPC result is success, but the response
      // contains an application-level error.
      rpc->RespondSuccess();
      return;
    }
    Status s = server_->ts_manager()->RegisterTS(req->common().ts_instance(),
                                                 req->registration(),
                                                 &ts_desc);
    if (!s.ok()) {
      LOG(WARNING) << Substitute("Unable to register tserver ($0): $1",
                                 rpc->requestor_string(), s.ToString());
      // TODO(todd): add service-specific errors
      rpc->RespondFailure(s);
      return;
    }
  } else {
    Status s = server_->ts_manager()->LookupTS(req->common().ts_instance(), &ts_desc);
    if (s.IsNotFound()) {
      LOG(INFO) << Substitute("Got heartbeat from unknown tserver ($0) as $1; "
          "Asking this server to re-register.",
          SecureShortDebugString(req->common().ts_instance()), rpc->requestor_string());
      resp->set_needs_reregister(true);

      // Don't bother asking for a full tablet report if we're a follower;
      // it'll just get ignored anyway.
      resp->set_needs_full_tablet_report(is_leader_master);

      rpc->RespondSuccess();
      return;
    }
    if (!s.ok()) {
      LOG(WARNING) << Substitute("Unable to look up tserver for heartbeat "
          "request $0 from $1: $2", SecureDebugString(*req),
          rpc->requestor_string(), s.ToString());
      rpc->RespondFailure(s.CloneAndPrepend("Unable to lookup tserver"));
      return;
    }
  }

  // 4. Update tserver soft state based on the heartbeat contents.
  ts_desc->UpdateHeartbeatTime();
  ts_desc->set_num_live_replicas(req->num_live_tablets());

  // 5. Only leaders handle tablet reports.
  if (is_leader_master && req->has_tablet_report()) {
    Status s = server_->catalog_manager()->ProcessTabletReport(
        ts_desc.get(), req->tablet_report(), resp->mutable_tablet_report(), rpc);
    if (!s.ok()) {
      rpc->RespondFailure(s.CloneAndPrepend("Failed to process tablet report"));
      return;
    }
  }

  // 6. Only leaders sign CSR from tablet servers (if present).
  if (is_leader_master && req->has_csr_der()) {
    string cert;
    Status s = server_->cert_authority()->SignServerCSR(
        req->csr_der(), rpc->remote_user(), &cert);
    if (!s.ok()) {
      rpc->RespondFailure(s.CloneAndPrepend("invalid CSR"));
      return;
    }
    LOG(INFO) << "Signed X509 certificate for tserver " << rpc->requestor_string();
    resp->mutable_signed_cert_der()->swap(cert);
    resp->add_ca_cert_der(server_->cert_authority()->ca_cert_der());
  }

  // 7. Only leaders send public parts of non-expired TSK which the TS doesn't
  //    have, except if the '--master_non_leader_masters_propagate_tsk'
  //    test-only flag is set.
  if ((is_leader_master ||
       PREDICT_FALSE(FLAGS_master_non_leader_masters_propagate_tsk)) &&
      req->has_latest_tsk_seq_num()) {
    auto tsk_public_keys = server_->token_signer()->verifier().ExportKeys(
        req->latest_tsk_seq_num());
    for (auto& key : tsk_public_keys) {
      resp->add_tsks()->Swap(&key);
    }
  }

  rpc->RespondSuccess();
}

void MasterServiceImpl::GetTabletLocations(const GetTabletLocationsRequestPB* req,
                                           GetTabletLocationsResponsePB* resp,
                                           rpc::RpcContext* rpc) {
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, rpc)) {
    return;
  }

  if (PREDICT_FALSE(FLAGS_master_inject_latency_on_tablet_lookups_ms > 0)) {
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_master_inject_latency_on_tablet_lookups_ms));
  }

  ServerRegistrationPB reg;
  vector<TSDescriptor*> locs;
  for (const string& tablet_id : req->tablet_ids()) {
    // TODO(todd): once we have catalog data. ACL checks would also go here, probably.
    TabletLocationsPB* locs_pb = resp->add_tablet_locations();
    Status s = server_->catalog_manager()->GetTabletLocations(
        tablet_id, req->replica_type_filter(), locs_pb);
    if (!s.ok()) {
      resp->mutable_tablet_locations()->RemoveLast();

      GetTabletLocationsResponsePB::Error* err = resp->add_errors();
      err->set_tablet_id(tablet_id);
      StatusToPB(s, err->mutable_status());
    }
  }

  rpc->RespondSuccess();
}

void MasterServiceImpl::CreateTable(const CreateTableRequestPB* req,
                                    CreateTableResponsePB* resp,
                                    rpc::RpcContext* rpc) {
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, rpc)) {
    return;
  }

  Status s = server_->catalog_manager()->CreateTable(req, resp, rpc);
  CheckRespErrorOrSetUnknown(s, resp);
  rpc->RespondSuccess();
}

void MasterServiceImpl::IsCreateTableDone(const IsCreateTableDoneRequestPB* req,
                                          IsCreateTableDoneResponsePB* resp,
                                          rpc::RpcContext* rpc) {
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, rpc)) {
    return;
  }

  Status s = server_->catalog_manager()->IsCreateTableDone(req, resp);
  CheckRespErrorOrSetUnknown(s, resp);
  rpc->RespondSuccess();
}

void MasterServiceImpl::DeleteTable(const DeleteTableRequestPB* req,
                                    DeleteTableResponsePB* resp,
                                    rpc::RpcContext* rpc) {
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, rpc)) {
    return;
  }

  Status s = server_->catalog_manager()->DeleteTableRpc(*req, resp, rpc);
  CheckRespErrorOrSetUnknown(s, resp);
  rpc->RespondSuccess();
}

void MasterServiceImpl::AlterTable(const AlterTableRequestPB* req,
                                   AlterTableResponsePB* resp,
                                   rpc::RpcContext* rpc) {
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, rpc)) {
    return;
  }

  Status s = server_->catalog_manager()->AlterTableRpc(*req, resp, rpc);
  CheckRespErrorOrSetUnknown(s, resp);
  rpc->RespondSuccess();
}

void MasterServiceImpl::IsAlterTableDone(const IsAlterTableDoneRequestPB* req,
                                         IsAlterTableDoneResponsePB* resp,
                                         rpc::RpcContext* rpc) {
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, rpc)) {
    return;
  }

  Status s = server_->catalog_manager()->IsAlterTableDone(req, resp, rpc);
  CheckRespErrorOrSetUnknown(s, resp);
  rpc->RespondSuccess();
}

void MasterServiceImpl::ListTables(const ListTablesRequestPB* req,
                                   ListTablesResponsePB* resp,
                                   rpc::RpcContext* rpc) {
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, rpc)) {
    return;
  }

  Status s = server_->catalog_manager()->ListTables(req, resp);
  CheckRespErrorOrSetUnknown(s, resp);
  rpc->RespondSuccess();
}

void MasterServiceImpl::GetTableLocations(const GetTableLocationsRequestPB* req,
                                          GetTableLocationsResponsePB* resp,
                                          rpc::RpcContext* rpc) {
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, rpc)) {
    return;
  }

  if (PREDICT_FALSE(FLAGS_master_inject_latency_on_tablet_lookups_ms > 0)) {
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_master_inject_latency_on_tablet_lookups_ms));
  }
  Status s = server_->catalog_manager()->GetTableLocations(req, resp);
  CheckRespErrorOrSetUnknown(s, resp);
  rpc->RespondSuccess();
}

void MasterServiceImpl::GetTableSchema(const GetTableSchemaRequestPB* req,
                                       GetTableSchemaResponsePB* resp,
                                       rpc::RpcContext* rpc) {
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, rpc)) {
    return;
  }

  Status s = server_->catalog_manager()->GetTableSchema(req, resp);
  CheckRespErrorOrSetUnknown(s, resp);
  rpc->RespondSuccess();
}

void MasterServiceImpl::ListTabletServers(const ListTabletServersRequestPB* req,
                                          ListTabletServersResponsePB* resp,
                                          rpc::RpcContext* rpc) {
  vector<std::shared_ptr<TSDescriptor> > descs;
  server_->ts_manager()->GetAllDescriptors(&descs);
  for (const std::shared_ptr<TSDescriptor>& desc : descs) {
    ListTabletServersResponsePB::Entry* entry = resp->add_servers();
    desc->GetNodeInstancePB(entry->mutable_instance_id());
    desc->GetRegistration(entry->mutable_registration());
    entry->set_millis_since_heartbeat(desc->TimeSinceHeartbeat().ToMilliseconds());
    if (desc->location()) entry->set_location(desc->location().get());
  }
  rpc->RespondSuccess();
}

void MasterServiceImpl::ListMasters(const ListMastersRequestPB* req,
                                    ListMastersResponsePB* resp,
                                    rpc::RpcContext* rpc) {
  vector<ServerEntryPB> masters;
  Status s = server_->ListMasters(&masters);
  if (!s.ok()) {
    StatusToPB(s, resp->mutable_error()->mutable_status());
    resp->mutable_error()->set_code(MasterErrorPB::UNKNOWN_ERROR);

    // Continue setting deprecated error status in order to maintain backwards compat.
    StatusToPB(s, resp->mutable_deprecated_error());
  } else {
    for (const ServerEntryPB& master : masters) {
      resp->add_masters()->CopyFrom(master);
    }
  }
  rpc->RespondSuccess();
}

void MasterServiceImpl::GetMasterRegistration(const GetMasterRegistrationRequestPB* req,
                                              GetMasterRegistrationResponsePB* resp,
                                              rpc::RpcContext* rpc) {
  // instance_id must always be set in order for status pages to be useful.
  resp->mutable_instance_id()->CopyFrom(server_->instance_pb());

  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedOrRespond(resp, rpc)) {
    return;
  }

  Status s = server_->GetMasterRegistration(resp->mutable_registration());
  CheckRespErrorOrSetUnknown(s, resp);
  resp->set_role(server_->catalog_manager()->Role());
  rpc->RespondSuccess();
}

void MasterServiceImpl::ConnectToMaster(const ConnectToMasterRequestPB* /*req*/,
                                        ConnectToMasterResponsePB* resp,
                                        rpc::RpcContext* rpc) {
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedOrRespond(resp, rpc)) {
    return;
  }

  // Set the info about the other masters, so that the client can verify
  // it has the full set of info.
  {
    vector<HostPortPB> hostports;
    WARN_NOT_OK(server_->GetMasterHostPorts(&hostports),
                "unable to get HostPorts for masters");
    resp->mutable_master_addrs()->Reserve(hostports.size());
    for (auto& hp : hostports) {
      *resp->add_master_addrs() = std::move(hp);
    }
  }

  const bool is_leader = l.leader_status().ok();
  if (is_leader) {
    resp->add_ca_cert_der(server_->cert_authority()->ca_cert_der());

    // Issue an authentication token for the caller, unless they are
    // already using a token to authenticate or haven't been authenticated
    // by other means. Don't issue a token if it's about to travel back to the
    // client over a non-confidential channel.
    if (rpc->is_confidential() &&
        rpc->remote_user().authenticated_by() != rpc::RemoteUser::AUTHN_TOKEN) {
      string username = rpc->remote_user().username();
      if (username.empty()) {
        static const char* const kErrMsg = "missing name of the remote user";
        StatusToPB(Status::InvalidArgument(kErrMsg),
                   resp->mutable_error()->mutable_status());
        resp->mutable_error()->set_code(MasterErrorPB::UNKNOWN_ERROR);
        rpc->RespondSuccess();
        KLOG_EVERY_N_SECS(WARNING, 60) << Substitute("invalid request from $0: $1",
                                                     rpc->requestor_string(), kErrMsg);
        return;
      }

      SignedTokenPB authn_token;
      Status s = server_->token_signer()->GenerateAuthnToken(username, &authn_token);
      if (!s.ok()) {
        LOG(FATAL) << Substitute("unable to generate signed token for $0: $1",
                                 rpc->requestor_string(), s.ToString());
      }

      // TODO(todd): this might be a good spot for some auditing code?
      resp->mutable_authn_token()->Swap(&authn_token);
    }
  }

  // Add Hive Metastore information.
  if (hms::HmsCatalog::IsEnabled()) {
    auto* metastore_config = resp->mutable_hms_config();
    metastore_config->set_hms_uris(FLAGS_hive_metastore_uris);
    metastore_config->set_hms_sasl_enabled(FLAGS_hive_metastore_sasl_enabled);
    // TODO(dan): set the hms_uuid field.
  }

  // Rather than consulting the current consensus role, instead base it
  // on the catalog manager's view. This prevents us from advertising LEADER
  // until we have taken over all the associated responsibilities.
  resp->set_role(is_leader ? consensus::RaftPeerPB::LEADER
                           : consensus::RaftPeerPB::FOLLOWER);
  rpc->RespondSuccess();
}

void MasterServiceImpl::ReplaceTablet(const ReplaceTabletRequestPB* req,
                                      ReplaceTabletResponsePB* resp,
                                      rpc::RpcContext* rpc) {
  LOG(INFO) << "ReplaceTablet: received request to replace tablet " << req->tablet_id()
            << " from " << rpc->requestor_string();

  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, rpc)) {
    return;
  }

  Status s = server_->catalog_manager()->ReplaceTablet(req->tablet_id(), resp);
  CheckRespErrorOrSetUnknown(s, resp);
  rpc->RespondSuccess();
}

bool MasterServiceImpl::SupportsFeature(uint32_t feature) const {
  switch (feature) {
    case MasterFeatures::RANGE_PARTITION_BOUNDS:    FALLTHROUGH_INTENDED;
    case MasterFeatures::ADD_DROP_RANGE_PARTITIONS: FALLTHROUGH_INTENDED;
    case MasterFeatures::REPLICA_MANAGEMENT:
      return true;
    case MasterFeatures::CONNECT_TO_MASTER:
      return FLAGS_master_support_connect_to_master_rpc;
    default:
      return false;
  }
}

} // namespace master
} // namespace kudu
