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

#include "kudu/tserver/tserver_path_handlers.h"

#include <algorithm>
#include <iosfwd>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/bind.hpp> // IWYU pragma: keep
#include <glog/logging.h>

#include "kudu/common/common.pb.h"
#include "kudu/common/iterator_stats.h"
#include "kudu/common/partition.h"
#include "kudu/common/wire_protocol.pb.h"
#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/log_anchor_registry.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/consensus/opid.pb.h"
#include "kudu/consensus/quorum_util.h"
#include "kudu/consensus/raft_consensus.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/gutil/strings/human_readable.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/numbers.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/server/webui_util.h"
#include "kudu/tablet/metadata.pb.h"
#include "kudu/tablet/tablet.h"
#include "kudu/tablet/tablet.pb.h"
#include "kudu/tablet/tablet_metadata.h"
#include "kudu/tablet/tablet_replica.h"
#include "kudu/tablet/transactions/transaction.h"
#include "kudu/tserver/scanners.h"
#include "kudu/tserver/tablet_server.h"
#include "kudu/tserver/ts_tablet_manager.h"
#include "kudu/util/easy_json.h"
#include "kudu/util/maintenance_manager.h"
#include "kudu/util/maintenance_manager.pb.h"
#include "kudu/util/monotime.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/url-coding.h"
#include "kudu/util/web_callback_registry.h"

using kudu::MaintenanceManagerStatusPB;
using kudu::consensus::ConsensusStatePB;
using kudu::consensus::GetConsensusRole;
using kudu::consensus::RaftPeerPB;
using kudu::consensus::TransactionStatusPB;
using kudu::pb_util::SecureDebugString;
using kudu::pb_util::SecureShortDebugString;
using kudu::tablet::Tablet;
using kudu::tablet::TabletReplica;
using kudu::tablet::TabletStatePB;
using kudu::tablet::TabletStatusPB;
using kudu::tablet::Transaction;
using std::endl;
using std::map;
using std::ostringstream;
using std::shared_ptr;
using std::string;
using std::vector;
using strings::Substitute;

namespace kudu {

class Schema;

namespace tserver {

namespace {

bool CompareByMemberType(const RaftPeerPB& a, const RaftPeerPB& b) {
  if (!a.has_member_type()) return false;
  if (!b.has_member_type()) return true;
  return a.member_type() < b.member_type();
}

string TabletLink(const string& id) {
  return Substitute("<a href=\"/tablet?id=$0\">$1</a>",
                    UrlEncodeToString(id),
                    EscapeForHtmlToString(id));
}

bool IsTombstoned(const scoped_refptr<TabletReplica>& replica) {
  return replica->data_state() == tablet::TABLET_DATA_TOMBSTONED;
}

string ConsensusStatePBToHtml(const ConsensusStatePB& cstate,
                              const string& local_uuid) {
  ostringstream html;

  html << "<ul>\n";
  std::vector<RaftPeerPB> sorted_peers;
  sorted_peers.assign(cstate.committed_config().peers().begin(),
                      cstate.committed_config().peers().end());
  std::sort(sorted_peers.begin(), sorted_peers.end(), &CompareByMemberType);
  for (const RaftPeerPB& peer : sorted_peers) {
    string peer_addr_or_uuid =
        peer.has_last_known_addr() ? Substitute("$0:$1",
                                                peer.last_known_addr().host(),
                                                peer.last_known_addr().port())
                                   : peer.permanent_uuid();
    peer_addr_or_uuid = EscapeForHtmlToString(peer_addr_or_uuid);
    string role_name = RaftPeerPB::Role_Name(GetConsensusRole(peer.permanent_uuid(), cstate));
    string formatted = Substitute("$0: $1", role_name, peer_addr_or_uuid);
    // Make the local peer bold.
    if (peer.permanent_uuid() == local_uuid) {
      formatted = Substitute("<b>$0</b>", formatted);
    }

    html << Substitute(" <li>$0</li>\n", formatted);
  }
  html << "</ul>\n";
  return html.str();
}

bool GetTabletID(const Webserver::WebRequest& req,
                 string* id,
                 Webserver::WebResponse* resp) {
  if (!FindCopy(req.parsed_args, "id", id)) {
    resp->status_code = HttpStatusCode::BadRequest;
    resp->output->Set("error", "Request missing 'id' argument");
    return false;
  }
  return true;
}

bool GetTabletReplica(TabletServer* tserver,
                      const Webserver::WebRequest& /*req*/,
                      scoped_refptr<TabletReplica>* replica,
                      const string& tablet_id,
                      Webserver::WebResponse* resp) {
  if (!tserver->tablet_manager()->LookupTablet(tablet_id, replica)) {
    resp->status_code = HttpStatusCode::NotFound;
    resp->output->Set("error",
                      Substitute("Tablet $0 not found", tablet_id));
    return false;
  }
  return true;
}

bool TabletBootstrapping(const scoped_refptr<TabletReplica>& replica,
                         const string& tablet_id,
                         Webserver::WebResponse* resp) {
  if (replica->state() == tablet::BOOTSTRAPPING) {
    resp->status_code = HttpStatusCode::ServiceUnavailable;
    resp->output->Set("error",
                      Substitute("Tablet $0 is still bootstrapping", tablet_id));
    return true;
  }
  return false;
}

// Returns true if the tablet_id was properly specified, the
// tablet is found, and is in a non-bootstrapping state.
bool LoadTablet(TabletServer* tserver,
                const Webserver::WebRequest& req,
                string* tablet_id, scoped_refptr<TabletReplica>* replica,
                Webserver::WebResponse* resp) {
  return GetTabletID(req, tablet_id, resp) &&
      GetTabletReplica(tserver, req, replica, *tablet_id, resp) &&
      !TabletBootstrapping(*replica, *tablet_id, resp);
}

} // anonymous namespace

TabletServerPathHandlers::~TabletServerPathHandlers() {
}

Status TabletServerPathHandlers::Register(Webserver* server) {
  server->RegisterPathHandler(
    "/scans", "Scans",
    boost::bind(&TabletServerPathHandlers::HandleScansPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);
  server->RegisterPathHandler(
    "/tablets", "Tablets",
    boost::bind(&TabletServerPathHandlers::HandleTabletsPage, this, _1, _2),
    true /* styled */, true /* is_on_nav_bar */);
  server->RegisterPathHandler(
    "/tablet", "",
    boost::bind(&TabletServerPathHandlers::HandleTabletPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);
  server->RegisterPrerenderedPathHandler(
    "/transactions", "",
    boost::bind(&TabletServerPathHandlers::HandleTransactionsPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);
  server->RegisterPathHandler(
    "/tablet-rowsetlayout-svg", "",
    boost::bind(&TabletServerPathHandlers::HandleTabletSVGPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);
  server->RegisterPathHandler(
    "/tablet-consensus-status", "",
    boost::bind(&TabletServerPathHandlers::HandleConsensusStatusPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);
  server->RegisterPathHandler(
    "/log-anchors", "",
    boost::bind(&TabletServerPathHandlers::HandleLogAnchorsPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);
  server->RegisterPrerenderedPathHandler(
    "/dashboards", "Dashboards",
    boost::bind(&TabletServerPathHandlers::HandleDashboardsPage, this, _1, _2),
    true /* styled */, true /* is_on_nav_bar */);
  server->RegisterPathHandler(
    "/maintenance-manager", "",
    boost::bind(&TabletServerPathHandlers::HandleMaintenanceManagerPage, this, _1, _2),
    true /* styled */, false /* is_on_nav_bar */);

  return Status::OK();
}

void TabletServerPathHandlers::HandleTransactionsPage(const Webserver::WebRequest& req,
                                                      Webserver::PrerenderedWebResponse* resp) {
  ostringstream* output = resp->output;
  bool as_text = ContainsKey(req.parsed_args, "raw");

  vector<scoped_refptr<TabletReplica> > replicas;
  tserver_->tablet_manager()->GetTabletReplicas(&replicas);

  string arg = FindWithDefault(req.parsed_args, "include_traces", "false");
  Transaction::TraceType trace_type = ParseLeadingBoolValue(
      arg.c_str(), false) ? Transaction::TRACE_TXNS : Transaction::NO_TRACE_TXNS;

  if (!as_text) {
    *output << "<h1>Transactions</h1>\n";
    *output << "<table class='table table-striped'>\n";
    *output << "   <thead><tr><th>Tablet id</th><th>Op Id</th>"
      "<th>Transaction Type</th><th>"
      "Total time in-flight</th><th>Description</th></tr></thead>\n";
    *output << "<tbody>\n";
  }

  for (const scoped_refptr<TabletReplica>& replica : replicas) {
    vector<TransactionStatusPB> inflight;

    if (replica->tablet() == nullptr) {
      continue;
    }

    replica->GetInFlightTransactions(trace_type, &inflight);
    for (const TransactionStatusPB& inflight_tx : inflight) {
      string total_time_str = Substitute("$0 us.", inflight_tx.running_for_micros());
      string description;
      if (trace_type == Transaction::TRACE_TXNS) {
        description = Substitute("$0, Trace: $1",
                                  inflight_tx.description(), inflight_tx.trace_buffer());
      } else {
        description = inflight_tx.description();
      }

      if (!as_text) {
        *output << Substitute(
          "<tr><th>$0</th><th>$1</th><th>$2</th><th>$3</th><th>$4</th></tr>\n",
          EscapeForHtmlToString(replica->tablet_id()),
          EscapeForHtmlToString(SecureShortDebugString(inflight_tx.op_id())),
          OperationType_Name(inflight_tx.tx_type()),
          total_time_str,
          EscapeForHtmlToString(description));
      } else {
        *output << "Tablet: " << replica->tablet_id() << endl;
        *output << "Op ID: " << SecureShortDebugString(inflight_tx.op_id()) << endl;
        *output << "Type: " << OperationType_Name(inflight_tx.tx_type()) << endl;
        *output << "Running: " << total_time_str;
        *output << description << endl;
        *output << endl;
      }
    }
  }

  if (!as_text) {
    *output << "</tbody></table>\n";
  }
}

void TabletServerPathHandlers::HandleTabletsPage(const Webserver::WebRequest& /*req*/,
                                                 Webserver::WebResponse* resp) {
  EasyJson* output = resp->output;
  vector<scoped_refptr<TabletReplica>> replicas;
  tserver_->tablet_manager()->GetTabletReplicas(&replicas);

  if (replicas.empty()) {
    (*output)["no_replicas"] = true;
    return;
  }

  // Sort by (table_name, tablet_id) tuples.
  std::sort(replicas.begin(), replicas.end(),
            [](const scoped_refptr<TabletReplica>& rep_a,
               const scoped_refptr<TabletReplica>& rep_b) {
              return std::make_pair(rep_a->tablet_metadata()->table_name(), rep_a->tablet_id()) <
                     std::make_pair(rep_b->tablet_metadata()->table_name(), rep_b->tablet_id());
            });

  // Populate the JSON object 'replicas_json' with information about the replicas
  // in 'replicas'
  const auto& local_uuid = tserver_->instance_pb().permanent_uuid();
  auto make_replicas_json =
      [&local_uuid](const vector<scoped_refptr<TabletReplica>>& replicas,
                    EasyJson* replicas_json) {
    map<string, int> statuses;
    for (const scoped_refptr<TabletReplica>& replica : replicas) {
      statuses[TabletStatePB_Name(replica->state())]++;
    }
    EasyJson statuses_json = replicas_json->Set("statuses", EasyJson::kArray);
    for (const auto& entry : statuses) {
      EasyJson status_json = statuses_json.PushBack(EasyJson::kObject);
      double percent = replicas.empty() ? 0 : (100.0 * entry.second) / replicas.size();
      status_json["status"] = entry.first;
      status_json["count"] = entry.second;
      status_json["percentage"] = StringPrintf("%.2f", percent);
    }
    (*replicas_json)["total_count"] = std::to_string(replicas.size());

    EasyJson details_json = replicas_json->Set("replicas", EasyJson::kArray);
    for (const scoped_refptr<TabletReplica>& replica : replicas) {
      EasyJson replica_json = details_json.PushBack(EasyJson::kObject);
      const auto* tablet = replica->tablet();
      const auto& tmeta = replica->tablet_metadata();
      TabletStatusPB status;
      replica->GetTabletStatusPB(&status);
      replica_json["table_name"] = status.table_name();
      if (tablet != nullptr) {
        replica_json["id_or_link"] = TabletLink(status.tablet_id());
      } else {
        replica_json["id_or_link"] = status.tablet_id();
      }
      replica_json["partition"] =
          tmeta->partition_schema().PartitionDebugString(tmeta->partition(),
                                                         tmeta->schema());
      replica_json["state"] = replica->HumanReadableState();
      if (status.has_estimated_on_disk_size()) {
        replica_json["n_bytes"] =
            HumanReadableNumBytes::ToString(status.estimated_on_disk_size());
      }
      // We don't show the config if it's a tombstone because it's misleading.
      string consensus_state_html;
      shared_ptr<consensus::RaftConsensus> consensus = replica->shared_consensus();
      if (!IsTombstoned(replica) && consensus) {
        ConsensusStatePB cstate;
        if (consensus->ConsensusState(&cstate).ok()) {
          replica_json["consensus_state_html"] = ConsensusStatePBToHtml(cstate,
                                                                        local_uuid);
        }
      }
    }
  };

  vector<scoped_refptr<TabletReplica>> live_replicas;
  vector<scoped_refptr<TabletReplica>> tombstoned_replicas;
  for (const scoped_refptr<TabletReplica>& replica : replicas) {
    if (IsTombstoned(replica)) {
      tombstoned_replicas.push_back(replica);
    } else {
      live_replicas.push_back(replica);
    }
  }

  if (!live_replicas.empty()) {
    EasyJson live_replicas_json = output->Set("live_replicas", EasyJson::kObject);
    make_replicas_json(live_replicas, &live_replicas_json);
  }
  if (!tombstoned_replicas.empty()) {
    EasyJson tombstoned_replicas_json = output->Set("tombstoned_replicas", EasyJson::kObject);
    make_replicas_json(tombstoned_replicas, &tombstoned_replicas_json);
  }
}

void TabletServerPathHandlers::HandleTabletPage(const Webserver::WebRequest& req,
                                                Webserver::WebResponse* resp) {
  string tablet_id;
  scoped_refptr<TabletReplica> replica;
  if (!LoadTablet(tserver_, req, &tablet_id, &replica, resp)) return;

  string table_name = replica->tablet_metadata()->table_name();
  RaftPeerPB::Role role = RaftPeerPB::UNKNOWN_ROLE;
  auto consensus = replica->consensus();
  if (consensus) {
    role = consensus->role();
  }

  EasyJson* output = resp->output;
  output->Set("tablet_id", tablet_id);
  output->Set("state", replica->HumanReadableState());
  output->Set("role", RaftPeerPB::Role_Name(role));
  output->Set("table_name", table_name);

  const auto& tmeta = replica->tablet_metadata();
  const Schema& schema = tmeta->schema();
  output->Set("partition",
              tmeta->partition_schema().PartitionDebugString(tmeta->partition(), schema));
  output->Set("on_disk_size", HumanReadableNumBytes::ToString(replica->OnDiskSize()));

  SchemaToJson(schema, output);
}

void TabletServerPathHandlers::HandleTabletSVGPage(const Webserver::WebRequest& req,
                                                   Webserver::WebResponse* resp) {
  string tablet_id;
  scoped_refptr<TabletReplica> replica;
  if (!LoadTablet(tserver_, req, &tablet_id, &replica, resp)) return;
  shared_ptr<Tablet> tablet = replica->shared_tablet();
  auto* output = resp->output;
  if (!tablet) {
    output->Set("error", Substitute("Tablet $0 is not running", tablet_id));
    return;
  }

  output->Set("tablet_id", tablet_id);
  ostringstream oss;
  tablet->PrintRSLayout(&oss);
  output->Set("rowset_layout", oss.str());
}

void TabletServerPathHandlers::HandleLogAnchorsPage(const Webserver::WebRequest& req,
                                                    Webserver::WebResponse* resp) {
  string tablet_id;
  scoped_refptr<TabletReplica> replica;
  if (!LoadTablet(tserver_, req, &tablet_id, &replica, resp)) return;

  auto* output = resp->output;
  output->Set("tablet_id", tablet_id);
  output->Set("log_anchors", replica->log_anchor_registry()->DumpAnchorInfo());
}

void TabletServerPathHandlers::HandleConsensusStatusPage(const Webserver::WebRequest& req,
                                                         Webserver::WebResponse* resp) {
  string tablet_id;
  scoped_refptr<TabletReplica> replica;
  if (!LoadTablet(tserver_, req, &tablet_id, &replica, resp)) return;
  shared_ptr<consensus::RaftConsensus> consensus = replica->shared_consensus();
  auto* output = resp->output;
  if (!consensus) {
    output->Set("error", Substitute("Tablet $0 not initialized", tablet_id));
    return;
  }
  ostringstream oss;
  consensus->DumpStatusHtml(oss);
  output->Set("consensus_status", oss.str());
}

namespace {
// Pretty-prints a scan's state.
const char* ScanStateToString(const ScanState& scan_state) {
  switch (scan_state) {
    case ScanState::kActive: return "Active";
    case ScanState::kComplete: return "Complete";
    case ScanState::kFailed: return "Failed";
    case ScanState::kExpired: return "Expired";
  }
  LOG(FATAL) << "missing ScanState branch";
}

// Formats the scan descriptor's pseudo-SQL query string as HTML.
string ScanQueryHtml(const ScanDescriptor& scan) {
  string query = "<b>SELECT</b> ";
  if (scan.projected_columns.empty()) {
    query.append("COUNT(*)");
  } else {
    query.append(JoinMapped(scan.projected_columns, EscapeForHtmlToString, ",<br>       "));
  }
  query.append("<br>  <b>FROM</b> ");
  if (scan.table_name.empty()) {
    query.append("&lt;unknown&gt;");
  } else {
    query.append(EscapeForHtmlToString(scan.table_name));
  }

  if (!scan.predicates.empty()) {
    query.append("<br> <b>WHERE</b> ");
    query.append(JoinMapped(scan.predicates, EscapeForHtmlToString, "<br>   <b>AND</b> "));
  }

  return query;
}

void IteratorStatsToJson(const ScanDescriptor& scan, EasyJson* json) {

  auto fill_stats = [] (EasyJson& row, const string& column, const IteratorStats& stats) {
    row["column"] = column;

    row["bytes_read"] = HumanReadableNumBytes::ToString(stats.bytes_read);
    row["cells_read"] = HumanReadableInt::ToString(stats.cells_read);
    row["blocks_read"] = HumanReadableInt::ToString(stats.blocks_read);

    row["bytes_read_title"] = stats.bytes_read;
    row["cells_read_title"] = stats.cells_read;
    row["blocks_read_title"] = stats.blocks_read;
  };

  IteratorStats total_stats;
  for (const auto& column : scan.iterator_stats) {
    EasyJson row = json->PushBack(EasyJson::kObject);
    fill_stats(row, column.first, column.second);
    total_stats += column.second;
  }

  EasyJson total_row = json->PushBack(EasyJson::kObject);
  fill_stats(total_row, "total", total_stats);
}

void ScanToJson(const ScanDescriptor& scan, EasyJson* json) {
  MonoTime now = MonoTime::Now();
  MonoDelta duration;
  if (scan.state == ScanState::kActive) {
    duration = now - scan.start_time;
  } else {
    duration = scan.last_access_time - scan.start_time;
  }
  MonoDelta time_since_start = now - scan.start_time;

  json->Set("tablet_id", scan.tablet_id);
  json->Set("scanner_id", scan.scanner_id);
  json->Set("state", ScanStateToString(scan.state));
  json->Set("query", ScanQueryHtml(scan));
  json->Set("requestor", scan.requestor);

  json->Set("duration", HumanReadableElapsedTime::ToShortString(duration.ToSeconds()));
  json->Set("time_since_start",
            HumanReadableElapsedTime::ToShortString(time_since_start.ToSeconds()));

  json->Set("duration_title", duration.ToSeconds());
  json->Set("time_since_start_title", time_since_start.ToSeconds());

  EasyJson stats_json = json->Set("stats", EasyJson::kArray);
  IteratorStatsToJson(scan, &stats_json);
}
} // anonymous namespace

void TabletServerPathHandlers::HandleScansPage(const Webserver::WebRequest& /*req*/,
                                               Webserver::WebResponse* resp) {
  EasyJson scans = resp->output->Set("scans", EasyJson::kArray);
  vector<ScanDescriptor> descriptors = tserver_->scanner_manager()->ListScans();

  for (const auto& descriptor : descriptors) {
    EasyJson scan = scans.PushBack(EasyJson::kObject);
    ScanToJson(descriptor, &scan);
  }
}

void TabletServerPathHandlers::HandleDashboardsPage(const Webserver::WebRequest& /*req*/,
                                                    Webserver::PrerenderedWebResponse* resp) {
  ostringstream* output = resp->output;
  *output << "<h3>Dashboards</h3>\n";
  *output << "<table class='table table-striped'>\n";
  *output << "  <thead><tr><th>Dashboard</th><th>Description</th></tr></thead>\n";
  *output << "  <tbody\n";
  *output << GetDashboardLine("scans", "Scans", "List of currently running and recently "
                                                "completed scans.");
  *output << GetDashboardLine("transactions", "Transactions", "List of transactions that are "
                                                              "currently running.");
  *output << GetDashboardLine("maintenance-manager", "Maintenance Manager",
                              "List of operations that are currently running and those "
                              "that are registered.");
  *output << "</tbody></table>\n";
}

string TabletServerPathHandlers::GetDashboardLine(const std::string& link,
                                                  const std::string& text,
                                                  const std::string& desc) {
  return Substitute("  <tr><td><a href=\"$0\">$1</a></td><td>$2</td></tr>\n",
                    EscapeForHtmlToString(link),
                    EscapeForHtmlToString(text),
                    EscapeForHtmlToString(desc));
}

void TabletServerPathHandlers::HandleMaintenanceManagerPage(const Webserver::WebRequest& req,
                                                            Webserver::WebResponse* resp) {
  EasyJson* output = resp->output;
  MaintenanceManager* manager = tserver_->maintenance_manager();
  MaintenanceManagerStatusPB pb;
  manager->GetMaintenanceManagerStatusDump(&pb);
  if (ContainsKey(req.parsed_args, "raw")) {
    (*output)["raw"] = SecureDebugString(pb);
    return;
  }

  EasyJson running_ops = output->Set("running_operations", EasyJson::kArray);
  for (const auto& op_pb : pb.registered_operations()) {
    if (op_pb.running() > 0) {
      EasyJson running_op = running_ops.PushBack(EasyJson::kObject);
      running_op["name"] = op_pb.name();
      running_op["instances_running"] = op_pb.running();
    }
  }

  EasyJson completed_ops = output->Set("completed_operations", EasyJson::kArray);
  for (const auto& op_pb : pb.completed_operations()) {
    EasyJson completed_op = completed_ops.PushBack(EasyJson::kObject);
    completed_op["name"] = op_pb.name();
    completed_op["duration"] =
      HumanReadableElapsedTime::ToShortString(op_pb.duration_millis() / 1000.0);
    completed_op["time_since_start"] =
      HumanReadableElapsedTime::ToShortString(op_pb.millis_since_start() / 1000.0);
  }

  EasyJson registered_ops = output->Set("registered_operations", EasyJson::kArray);
  for (const auto& op_pb : pb.registered_operations()) {
    EasyJson registered_op = registered_ops.PushBack(EasyJson::kObject);
    registered_op["name"] = op_pb.name();
    registered_op["runnable"] = op_pb.runnable();
    registered_op["ram_anchored"] = HumanReadableNumBytes::ToString(op_pb.ram_anchored_bytes());
    registered_op["logs_retained"] = HumanReadableNumBytes::ToString(op_pb.logs_retained_bytes());
    registered_op["perf"] = op_pb.perf_improvement();
  }
}

} // namespace tserver
} // namespace kudu
