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
#ifndef KUDU_TSERVER_TABLET_COPY_SERVICE_H_
#define KUDU_TSERVER_TABLET_COPY_SERVICE_H_

#include <string>
#include <unordered_map>

#include "kudu/gutil/port.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/tserver/tablet_copy.pb.h"
#include "kudu/tserver/tablet_copy.service.h"
#include "kudu/tserver/tablet_copy_source_session.h"
#include "kudu/util/countdown_latch.h"
#include "kudu/util/monotime.h"
#include "kudu/util/mutex.h"
#include "kudu/util/random.h"
#include "kudu/util/status.h"
#include "kudu/util/thread.h"

namespace google {
namespace protobuf {
class Message;
}
}

namespace kudu {

class FsManager;

namespace server {
class ServerBase;
} // namespace server

namespace rpc {
class RpcContext;
} // namespace rpc

namespace tserver {

class TabletReplicaLookupIf;

class TabletCopyServiceImpl : public TabletCopyServiceIf {
 public:
  TabletCopyServiceImpl(server::ServerBase* server,
                        TabletReplicaLookupIf* tablet_replica_lookup);

  bool AuthorizeServiceUser(const google::protobuf::Message* req,
                            google::protobuf::Message* resp,
                            rpc::RpcContext* rpc) override;

  virtual void BeginTabletCopySession(const BeginTabletCopySessionRequestPB* req,
                                           BeginTabletCopySessionResponsePB* resp,
                                           rpc::RpcContext* context) OVERRIDE;

  virtual void CheckSessionActive(const CheckTabletCopySessionActiveRequestPB* req,
                                  CheckTabletCopySessionActiveResponsePB* resp,
                                  rpc::RpcContext* context) OVERRIDE;

  virtual void FetchData(const FetchDataRequestPB* req,
                         FetchDataResponsePB* resp,
                         rpc::RpcContext* context) OVERRIDE;

  virtual void EndTabletCopySession(const EndTabletCopySessionRequestPB* req,
                                         EndTabletCopySessionResponsePB* resp,
                                         rpc::RpcContext* context) OVERRIDE;

  virtual void Shutdown() OVERRIDE;

 private:
  struct SessionEntry {
    explicit SessionEntry(scoped_refptr<TabletCopySourceSession> session_in);

    scoped_refptr<TabletCopySourceSession> session;
    MonoTime last_accessed_time; // Time this session was last accessed.
    MonoDelta expire_timeout;
  };

  typedef std::unordered_map<std::string, SessionEntry> SessionMap;

  // Look up session in session map.
  Status FindSessionUnlocked(const std::string& session_id,
                             TabletCopyErrorPB::Code* app_error,
                             scoped_refptr<TabletCopySourceSession>* session) const;

  // Validate the data identifier in a FetchData request.
  Status ValidateFetchRequestDataId(const DataIdPB& data_id,
                                    TabletCopyErrorPB::Code* app_error,
                                    const scoped_refptr<TabletCopySourceSession>& session) const;

  // Take note of session activity; Re-update the session timeout deadline.
  void ResetSessionExpirationUnlocked(const std::string& session_id);

  // Destroy the specified tablet copy session.
  Status DoEndTabletCopySessionUnlocked(const std::string& session_id,
                                             TabletCopyErrorPB::Code* app_error);

  // The timeout thread periodically checks whether sessions are expired and
  // removes them from the map.
  void EndExpiredSessions();

  std::string LogPrefix() const;

  void SetupErrorAndRespond(rpc::RpcContext* context,
                            TabletCopyErrorPB::Code code,
                            const std::string& message,
                            const Status& s);

  server::ServerBase* server_;
  FsManager* fs_manager_;
  TabletReplicaLookupIf* tablet_replica_lookup_;

  // Protects sessions_ map.
  mutable Mutex sessions_lock_;
  SessionMap sessions_;
  ThreadSafeRandom rand_;

  // Session expiration thread.
  // TODO(mpercy): This is a hack, replace some kind of timer. See KUDU-286.
  CountDownLatch shutdown_latch_;
  scoped_refptr<Thread> session_expiration_thread_;

  TabletCopySourceMetrics tablet_copy_metrics_;
};

} // namespace tserver
} // namespace kudu

#endif // KUDU_TSERVER_TABLET_COPY_SERVICE_H_
