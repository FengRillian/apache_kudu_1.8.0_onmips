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

#include "kudu/tools/tool_action_common.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <google/protobuf/util/json_util.h>

#include "kudu/client/client-internal.h"  // IWYU pragma: keep
#include "kudu/client/client.h"
#include "kudu/client/shared_ptr.h"
#include "kudu/common/common.pb.h"
#include "kudu/common/row_operations.h"
#include "kudu/common/schema.h"
#include "kudu/common/wire_protocol.h"
#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/consensus.proxy.h" // IWYU pragma: keep
#include "kudu/consensus/log.pb.h"
#include "kudu/consensus/log_util.h"
#include "kudu/consensus/opid.pb.h"
#include "kudu/gutil/endian.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/numbers.h"
#include "kudu/gutil/strings/split.h"
#include "kudu/gutil/strings/stringpiece.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/master/master.proxy.h" // IWYU pragma: keep
#include "kudu/rpc/messenger.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/rpc/rpc_header.pb.h"
#include "kudu/server/server_base.pb.h"
#include "kudu/server/server_base.proxy.h"
#include "kudu/tools/tool.pb.h" // IWYU pragma: keep
#include "kudu/tools/tool_action.h"
#include "kudu/tserver/tserver.pb.h"
#include "kudu/tserver/tserver_admin.proxy.h"   // IWYU pragma: keep
#include "kudu/tserver/tserver_service.proxy.h" // IWYU pragma: keep
#include "kudu/util/faststring.h"
#include "kudu/util/jsonwriter.h"
#include "kudu/util/memory/arena.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/net/sockaddr.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/status.h"

DEFINE_bool(force, false, "If true, allows the set_flag command to set a flag "
            "which is not explicitly marked as runtime-settable. Such flag "
            "changes may be simply ignored on the server, or may cause the "
            "server to crash.");
DEFINE_bool(print_meta, true, "Include metadata in output");
DEFINE_string(print_entries, "decoded",
              "How to print entries:\n"
              "  false|0|no = don't print\n"
              "  true|1|yes|decoded = print them decoded\n"
              "  pb = print the raw protobuf\n"
              "  id = print only their ids");
DEFINE_string(table_name, "",
              "Restrict output to a specific table by name");
DEFINE_int64(timeout_ms, 1000 * 60, "RPC timeout in milliseconds");
DEFINE_int32(truncate_data, 100,
             "Truncate the data fields to the given number of bytes "
             "before printing. Set to 0 to disable");

DEFINE_string(columns, "", "Comma-separated list of column fields to include in output tables");
DEFINE_string(format, "pretty",
              "Format to use for printing list output tables.\n"
              "Possible values: pretty, space, tsv, csv, and json");

DEFINE_string(flag_tags, "", "Comma-separated list of tags used to restrict which "
                             "flags are returned. An empty value matches all tags");
DEFINE_bool(all_flags, false, "Whether to return all flags, or only flags that "
                              "were explicitly set.");
DEFINE_string(tables, "", "Tables to include (comma-separated list of table names)"
                          "If not specified, includes all tables.");

namespace boost {
template <typename Signature>
class function;
} // namespace boost

namespace kudu {

namespace master {
class ListMastersRequestPB;
class ListMastersResponsePB;
class ListTabletServersRequestPB;
class ListTabletServersResponsePB;
class ReplaceTabletRequestPB;
class ReplaceTabletResponsePB;
} // namespace master

namespace tools {

using client::KuduClient;
using client::KuduClientBuilder;
using client::KuduTablet;
using client::KuduTabletServer;
using consensus::ConsensusServiceProxy;
using consensus::ReplicateMsg;
using log::LogEntryPB;
using log::LogEntryReader;
using log::ReadableLogSegment;
using master::ListMastersRequestPB;
using master::ListMastersResponsePB;
using master::ListTabletServersRequestPB;
using master::ListTabletServersResponsePB;
using master::MasterServiceProxy;
using master::ReplaceTabletRequestPB;
using master::ReplaceTabletResponsePB;
using pb_util::SecureDebugString;
using pb_util::SecureShortDebugString;
using rpc::Messenger;
using rpc::MessengerBuilder;
using rpc::RequestIdPB;
using rpc::RpcController;
using server::GenericServiceProxy;
using server::GetFlagsRequestPB;
using server::GetFlagsResponsePB;
using server::GetStatusRequestPB;
using server::GetStatusResponsePB;
using server::ServerClockRequestPB;
using server::ServerClockResponsePB;
using server::ServerStatusPB;
using server::SetFlagRequestPB;
using server::SetFlagResponsePB;
using std::cout;
using std::endl;
using std::ostream;
using std::setfill;
using std::setw;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;
using strings::Substitute;
using tserver::TabletServerAdminServiceProxy;
using tserver::TabletServerServiceProxy;
using tserver::WriteRequestPB;

const char* const kMasterAddressesArg = "master_addresses";
const char* const kMasterAddressesArgDesc = "Comma-separated list of Kudu "
    "Master addresses where each address is of form 'hostname:port'";
const char* const kTabletIdArg = "tablet_id";
const char* const kTabletIdArgDesc = "Tablet Identifier";

namespace {

enum PrintEntryType {
  DONT_PRINT,
  PRINT_PB,
  PRINT_DECODED,
  PRINT_ID
};

PrintEntryType ParsePrintType() {
  if (ParseLeadingBoolValue(FLAGS_print_entries.c_str(), true) == false) {
    return DONT_PRINT;
  } else if (ParseLeadingBoolValue(FLAGS_print_entries.c_str(), false) == true ||
             FLAGS_print_entries == "decoded") {
    return PRINT_DECODED;
  } else if (FLAGS_print_entries == "pb") {
    return PRINT_PB;
  } else if (FLAGS_print_entries == "id") {
    return PRINT_ID;
  } else {
    LOG(FATAL) << "Unknown value for --print_entries: " << FLAGS_print_entries;
  }
}

void PrintIdOnly(const LogEntryPB& entry) {
  switch (entry.type()) {
    case log::REPLICATE:
    {
      cout << entry.replicate().id().term() << "." << entry.replicate().id().index()
           << "@" << entry.replicate().timestamp() << "\t";
      cout << "REPLICATE "
           << OperationType_Name(entry.replicate().op_type());
      break;
    }
    case log::COMMIT:
    {
      cout << "COMMIT " << entry.commit().commited_op_id().term()
           << "." << entry.commit().commited_op_id().index();
      break;
    }
    default:
      cout << "UNKNOWN: " << SecureShortDebugString(entry);
  }

  cout << endl;
}

Status PrintDecodedWriteRequestPB(const string& indent,
                                  const Schema& tablet_schema,
                                  const WriteRequestPB& write,
                                  const RequestIdPB* request_id) {
  Schema request_schema;
  RETURN_NOT_OK(SchemaFromPB(write.schema(), &request_schema));

  Arena arena(32 * 1024);
  RowOperationsPBDecoder dec(&write.row_operations(), &request_schema, &tablet_schema, &arena);
  vector<DecodedRowOperation> ops;
  RETURN_NOT_OK(dec.DecodeOperations(&ops));

  cout << indent << "Tablet: " << write.tablet_id() << endl;
  cout << indent << "RequestId: "
      << (request_id ? SecureShortDebugString(*request_id) : "None") << endl;
  cout << indent << "Consistency: "
       << ExternalConsistencyMode_Name(write.external_consistency_mode()) << endl;
  if (write.has_propagated_timestamp()) {
    cout << indent << "Propagated TS: " << write.propagated_timestamp() << endl;
  }

  int i = 0;
  for (const DecodedRowOperation& op : ops) {
    // TODO (KUDU-515): Handle the case when a tablet's schema changes
    // mid-segment.
    cout << indent << "op " << (i++) << ": " << op.ToString(tablet_schema) << endl;
  }

  return Status::OK();
}

Status PrintDecoded(const LogEntryPB& entry, const Schema& tablet_schema) {
  PrintIdOnly(entry);

  const string indent = "\t";
  if (entry.has_replicate()) {
    // We can actually decode REPLICATE messages.

    const ReplicateMsg& replicate = entry.replicate();
    if (replicate.op_type() == consensus::WRITE_OP) {
      RETURN_NOT_OK(PrintDecodedWriteRequestPB(
          indent,
          tablet_schema,
          replicate.write_request(),
          replicate.has_request_id() ? &replicate.request_id() : nullptr));
    } else {
      cout << indent << SecureShortDebugString(replicate) << endl;
    }
  } else if (entry.has_commit()) {
    // For COMMIT we'll just dump the PB
    cout << indent << SecureShortDebugString(entry.commit()) << endl;
  }

  return Status::OK();
}

} // anonymous namespace

template<class ProxyClass>
Status BuildProxy(const string& address,
                  uint16_t default_port,
                  unique_ptr<ProxyClass>* proxy) {
  HostPort hp;
  RETURN_NOT_OK(hp.ParseString(address, default_port));
  shared_ptr<Messenger> messenger;
  RETURN_NOT_OK(MessengerBuilder("tool").Build(&messenger));

  vector<Sockaddr> resolved;
  RETURN_NOT_OK(hp.ResolveAddresses(&resolved));

  proxy->reset(new ProxyClass(messenger, resolved[0], hp.host()));
  return Status::OK();
}

// Explicit specialization for callers outside this compilation unit.
template
Status BuildProxy(const string& address,
                  uint16_t default_port,
                  unique_ptr<ConsensusServiceProxy>* proxy);
template
Status BuildProxy(const string& address,
                  uint16_t default_port,
                  unique_ptr<TabletServerServiceProxy>* proxy);
template
Status BuildProxy(const string& address,
                  uint16_t default_port,
                  unique_ptr<TabletServerAdminServiceProxy>* proxy);
template
Status BuildProxy(const string& address,
                  uint16_t default_port,
                  unique_ptr<MasterServiceProxy>* proxy);

Status GetServerStatus(const string& address, uint16_t default_port,
                       ServerStatusPB* status) {
  unique_ptr<GenericServiceProxy> proxy;
  RETURN_NOT_OK(BuildProxy(address, default_port, &proxy));

  GetStatusRequestPB req;
  GetStatusResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_timeout_ms));

  RETURN_NOT_OK(proxy->GetStatus(req, &resp, &rpc));
  if (!resp.has_status()) {
    return Status::Incomplete("Server response did not contain status",
                              proxy->ToString());
  }
  *status = resp.status();
  return Status::OK();
}

Status PrintSegment(const scoped_refptr<ReadableLogSegment>& segment) {
  PrintEntryType print_type = ParsePrintType();
  if (FLAGS_print_meta) {
    cout << "Header:\n" << SecureDebugString(segment->header());
  }
  if (print_type != DONT_PRINT) {
    Schema tablet_schema;
    RETURN_NOT_OK(SchemaFromPB(segment->header().schema(), &tablet_schema));

    LogEntryReader reader(segment.get());
    while (true) {
      unique_ptr<LogEntryPB> entry;
      Status s = reader.ReadNextEntry(&entry);
      if (s.IsEndOfFile()) {
        break;
      }
      RETURN_NOT_OK(s);

      if (print_type == PRINT_PB) {
        if (FLAGS_truncate_data > 0) {
          pb_util::TruncateFields(entry.get(), FLAGS_truncate_data);
        }

        cout << "Entry:\n" << SecureDebugString(*entry);
      } else if (print_type == PRINT_DECODED) {
        RETURN_NOT_OK(PrintDecoded(*entry, tablet_schema));
      } else if (print_type == PRINT_ID) {
        PrintIdOnly(*entry);
      }
    }
  }
  if (FLAGS_print_meta && segment->HasFooter()) {
    cout << "Footer:\n" << SecureDebugString(segment->footer());
  }

  return Status::OK();
}

Status GetServerFlags(const std::string& address,
                      uint16_t default_port,
                      bool all_flags,
                      const std::string& flag_tags,
                      std::vector<server::GetFlagsResponsePB_Flag>* flags) {
  unique_ptr<GenericServiceProxy> proxy;
  RETURN_NOT_OK(BuildProxy(address, default_port, &proxy));

  GetFlagsRequestPB req;
  GetFlagsResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_timeout_ms));

  req.set_all_flags(all_flags);
  for (StringPiece tag : strings::Split(flag_tags, ",", strings::SkipEmpty())) {
    req.add_tags(tag.as_string());
  }
  RETURN_NOT_OK(proxy->GetFlags(req, &resp, &rpc));

  flags->clear();
  std::move(resp.flags().begin(), resp.flags().end(), std::back_inserter(*flags));
  return Status::OK();
}

Status PrintServerFlags(const string& address, uint16_t default_port) {
  vector<server::GetFlagsResponsePB_Flag> flags;
  RETURN_NOT_OK(GetServerFlags(address, default_port, FLAGS_all_flags, FLAGS_flag_tags, &flags));

  std::sort(flags.begin(), flags.end(),
      [](const GetFlagsResponsePB::Flag& left,
         const GetFlagsResponsePB::Flag& right) -> bool {
        return left.name() < right.name();
      });
  DataTable table({ "flag", "value", "default value?", "tags" });
  vector<string> tags;
  for (const auto& flag : flags) {
    tags.clear();
    std::copy(flag.tags().begin(), flag.tags().end(), std::back_inserter(tags));
    std::sort(tags.begin(), tags.end());
    table.AddRow({ flag.name(),
                   flag.value(),
                   flag.is_default_value() ? "true" : "false",
                   JoinStrings(tags, ",") });
  }
  return table.PrintTo(cout);
}

Status SetServerFlag(const string& address, uint16_t default_port,
                     const string& flag, const string& value) {
  unique_ptr<GenericServiceProxy> proxy;
  RETURN_NOT_OK(BuildProxy(address, default_port, &proxy));

  SetFlagRequestPB req;
  SetFlagResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_timeout_ms));

  req.set_flag(flag);
  req.set_value(value);
  req.set_force(FLAGS_force);

  RETURN_NOT_OK(proxy->SetFlag(req, &resp, &rpc));
  switch (resp.result()) {
    case server::SetFlagResponsePB::SUCCESS:
      return Status::OK();
    case server::SetFlagResponsePB::NOT_SAFE:
      return Status::RemoteError(resp.msg() +
                                 " (use --force flag to allow anyway)");
    default:
      return Status::RemoteError(SecureShortDebugString(resp));
  }
}

string GetMasterAddresses(const client::KuduClient& client) {
  return HostPort::ToCommaSeparatedString(client.data_->master_hostports());
}

bool MatchesAnyPattern(const vector<string>& patterns, const string& str) {
  // Consider no filter a wildcard.
  if (patterns.empty()) return true;

  for (const auto& p : patterns) {
    if (MatchPattern(str, p)) return true;
  }
  return false;
}

Status PrintServerStatus(const string& address, uint16_t default_port) {
  ServerStatusPB status;
  RETURN_NOT_OK(GetServerStatus(address, default_port, &status));
  cout << SecureDebugString(status) << endl;
  return Status::OK();
}

Status PrintServerTimestamp(const string& address, uint16_t default_port) {
  unique_ptr<GenericServiceProxy> proxy;
  RETURN_NOT_OK(BuildProxy(address, default_port, &proxy));

  ServerClockRequestPB req;
  ServerClockResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_timeout_ms));
  RETURN_NOT_OK(proxy->ServerClock(req, &resp, &rpc));
  if (!resp.has_timestamp()) {
    return Status::Incomplete("Server response did not contain timestamp",
                              proxy->ToString());
  }
  cout << resp.timestamp() << endl;
  return Status::OK();
}

namespace {

// Pretty print a table using the psql format. For example:
//
//                uuid               |         rpc-addresses          |      seqno
// ----------------------------------+--------------------------------+------------------
//  335d132897de4bdb9b87443f2c487a42 | 126.rack1.dc1.example.com:7050 | 1492596790237811
//  7425c65d80f54f2da0a85494a5eb3e68 | 122.rack1.dc1.example.com:7050 | 1492596755322350
//  dd23284d3a334f1a8306c19d89c1161f | 130.rack1.dc1.example.com:7050 | 1492596704536543
//  d8009e07d82b4e66a7ab50f85e60bc30 | 136.rack1.dc1.example.com:7050 | 1492596696557549
//  c108a85a68504c2bb9f49e4ee683d981 | 128.rack1.dc1.example.com:7050 | 1492596646623301
void PrettyPrintTable(const vector<string>& headers,
                      const vector<vector<string>>& columns,
                      ostream& out) {
  CHECK_EQ(headers.size(), columns.size());
  if (headers.empty()) return;
  size_t num_columns = headers.size();

  vector<size_t> widths;
  for (int col = 0; col < num_columns; col++) {
    size_t width = std::accumulate(columns[col].begin(), columns[col].end(), headers[col].size(),
                                   [](size_t acc, const string& cell) {
                                     return std::max(acc, cell.size());
                                   });
    widths.push_back(width);
  }

  // Print the header row.
  for (int col = 0; col < num_columns; col++) {
    int padding = widths[col] - headers[col].size();
    out << setw(padding / 2) << "" << " " << headers[col];
    if (col != num_columns - 1) out << setw((padding + 1) / 2) << "" << " |";
  }
  out << endl;

  // Print the separator row.
  out << setfill('-');
  for (int col = 0; col < num_columns; col++) {
    out << setw(widths[col] + 2) << "";
    if (col != num_columns - 1) out << "+";
  }
  out << endl;

  // Print the data rows.
  out << setfill(' ');
  int num_rows = columns.empty() ? 0 : columns[0].size();
  for (int row = 0; row < num_rows; row++) {
    for (int col = 0; col < num_columns; col++) {
      const auto& value = columns[col][row];
      out << " " << value;
      if (col != num_columns - 1) {
        size_t padding = widths[col] - value.size();
        out << setw(padding) << "" << " |";
      }
    }
    out << endl;
  }
}

// Print a table using JSON formatting.
//
// The table is formatted as an array of objects. Each object corresponds
// to a row whose fields are the column values.
void JsonPrintTable(const vector<string>& headers,
                    const vector<vector<string>>& columns,
                    ostream& out) {
  std::ostringstream stream;
  JsonWriter writer(&stream, JsonWriter::COMPACT);

  int num_columns = columns.size();
  int num_rows = columns.empty() ? 0 : columns[0].size();

  writer.StartArray();
  for (int row = 0; row < num_rows; row++) {
    writer.StartObject();
    for (int col = 0; col < num_columns; col++) {
      writer.String(headers[col]);
      writer.String(columns[col][row]);
    }
    writer.EndObject();
  }
  writer.EndArray();

  out << stream.str() << endl;
}

// Print the table using the provided separator. For example, with a comma
// separator:
//
// 335d132897de4bdb9b87443f2c487a42,126.rack1.dc1.example.com:7050,1492596790237811
// 7425c65d80f54f2da0a85494a5eb3e68,122.rack1.dc1.example.com:7050,1492596755322350
// dd23284d3a334f1a8306c19d89c1161f,130.rack1.dc1.example.com:7050,1492596704536543
// d8009e07d82b4e66a7ab50f85e60bc30,136.rack1.dc1.example.com:7050,1492596696557549
// c108a85a68504c2bb9f49e4ee683d981,128.rack1.dc1.example.com:7050,1492596646623301
void PrintTable(const vector<vector<string>>& columns, const string& separator, ostream& out) {
  // TODO(dan): proper escaping of string values.
  int num_columns = columns.size();
  int num_rows = columns.empty() ? 0 : columns[0].size();
  for (int row = 0; row < num_rows; row++) {
      for (int col = 0; col < num_columns; col++) {
        out << columns[col][row];
        if (col != num_columns - 1) out << separator;
      }
      out << endl;
  }
}

} // anonymous namespace

DataTable::DataTable(std::vector<string> col_names)
    : column_names_(std::move(col_names)),
      columns_(column_names_.size()) {
}

void DataTable::AddRow(std::vector<string> row) {
  CHECK_EQ(row.size(), columns_.size());
  int i = 0;
  for (auto& v : row) {
    columns_[i++].emplace_back(std::move(v));
  }
}

void DataTable::AddColumn(string name, vector<string> column) {
  if (!columns_.empty()) {
    CHECK_EQ(column.size(), columns_[0].size());
  }
  column_names_.emplace_back(std::move(name));
  columns_.emplace_back(std::move(column));
}

Status DataTable::PrintTo(ostream& out) const {
  if (boost::iequals(FLAGS_format, "pretty")) {
    PrettyPrintTable(column_names_, columns_, out);
  } else if (boost::iequals(FLAGS_format, "space")) {
    PrintTable(columns_, " ", out);
  } else if (boost::iequals(FLAGS_format, "tsv")) {
    PrintTable(columns_, "	", out);
  } else if (boost::iequals(FLAGS_format, "csv")) {
    PrintTable(columns_, ",", out);
  } else if (boost::iequals(FLAGS_format, "json")) {
    JsonPrintTable(column_names_, columns_, out);
  } else {
    return Status::InvalidArgument("unknown format (--format)", FLAGS_format);
  }
  return Status::OK();
}

LeaderMasterProxy::LeaderMasterProxy(client::sp::shared_ptr<KuduClient> client) :
  client_(std::move(client)) {
}

Status LeaderMasterProxy::Init(const vector<string>& master_addrs, const MonoDelta& timeout) {
  return KuduClientBuilder().master_server_addrs(master_addrs)
                            .default_rpc_timeout(timeout)
                            .default_admin_operation_timeout(timeout)
                            .Build(&client_);
}

Status LeaderMasterProxy::Init(const RunnerContext& context) {
  const string& master_addrs = FindOrDie(context.required_args, kMasterAddressesArg);
  return Init(strings::Split(master_addrs, ","), MonoDelta::FromMilliseconds(FLAGS_timeout_ms));
}

template<typename Req, typename Resp>
Status LeaderMasterProxy::SyncRpc(const Req& req,
                                  Resp* resp,
                                  const char* func_name,
                                  const boost::function<Status(master::MasterServiceProxy*,
                                                               const Req&, Resp*,
                                                               rpc::RpcController*)>& func) {
  MonoTime deadline = MonoTime::Now() + MonoDelta::FromMilliseconds(FLAGS_timeout_ms);
  return client_->data_->SyncLeaderMasterRpc(deadline, client_.get(), req, resp,
                                             func_name, func, {});
}

// Explicit specializations for callers outside this compilation unit.
template
Status LeaderMasterProxy::SyncRpc(const ListTabletServersRequestPB& req,
                                  ListTabletServersResponsePB* resp,
                                  const char* func_name,
                                  const boost::function<Status(MasterServiceProxy*,
                                                               const ListTabletServersRequestPB&,
                                                               ListTabletServersResponsePB*,
                                                               RpcController*)>& func);
template
Status LeaderMasterProxy::SyncRpc(const ListMastersRequestPB& req,
                                  ListMastersResponsePB* resp,
                                  const char* func_name,
                                  const boost::function<Status(MasterServiceProxy*,
                                                               const ListMastersRequestPB&,
                                                               ListMastersResponsePB*,
                                                               RpcController*)>& func);
template
Status LeaderMasterProxy::SyncRpc(const ReplaceTabletRequestPB& req,
                                  ReplaceTabletResponsePB* resp,
                                  const char* func_name,
                                  const boost::function<Status(MasterServiceProxy*,
                                                               const ReplaceTabletRequestPB&,
                                                               ReplaceTabletResponsePB*,
                                                               RpcController*)>& func);

const int ControlShellProtocol::kMaxMessageBytes = 1024 * 1024;

ControlShellProtocol::ControlShellProtocol(SerializationMode serialization_mode,
                                           CloseMode close_mode,
                                           int read_fd,
                                           int write_fd)
    : serialization_mode_(serialization_mode),
      close_mode_(close_mode),
      read_fd_(read_fd),
      write_fd_(write_fd) {
}

ControlShellProtocol::~ControlShellProtocol() {
  if (close_mode_ == CloseMode::CLOSE_ON_DESTROY) {
    int ret;
    RETRY_ON_EINTR(ret, close(read_fd_));
    RETRY_ON_EINTR(ret, close(write_fd_));
  }
}

template <class M>
Status ControlShellProtocol::ReceiveMessage(M* message) {
  switch (serialization_mode_) {
    case SerializationMode::JSON:
    {
      // Read and accumulate one byte at a time, looking for the newline.
      //
      // TODO(adar): it would be more efficient to read a chunk of data, look
      // for a newline, and if found, store the remainder for the next message.
      faststring buf;
      faststring one_byte;
      one_byte.resize(1);
      while (true) {
        RETURN_NOT_OK_PREPEND(DoRead(&one_byte), "unable to receive message byte");
        if (one_byte[0] == '\n') {
          break;
        }
        buf.push_back(one_byte[0]);
      }

      // Parse the JSON-encoded message.
      const auto& google_status =
          google::protobuf::util::JsonStringToMessage(buf.ToString(), message);
      if (!google_status.ok()) {
        return Status::InvalidArgument(
            Substitute("unable to parse JSON: $0", buf.ToString()),
            google_status.error_message().ToString());
      }
      break;
    }
    case SerializationMode::PB:
    {
      // Read four bytes of size (big-endian).
      faststring size_buf;
      size_buf.resize(sizeof(uint32_t));
      RETURN_NOT_OK_PREPEND(DoRead(&size_buf), "unable to receive message size");
      uint32_t body_size = NetworkByteOrder::Load32(size_buf.data());

      if (body_size > kMaxMessageBytes) {
        return Status::IOError(
            Substitute("message size ($0) exceeds maximum message size ($1)",
                       body_size, kMaxMessageBytes));
      }

      // Read the variable size body.
      faststring body_buf;
      body_buf.resize(body_size);
      RETURN_NOT_OK_PREPEND(DoRead(&body_buf), "unable to receive message body");

      // Parse the body into a PB request.
      RETURN_NOT_OK_PREPEND(pb_util::ParseFromArray(
          message, body_buf.data(), body_buf.length()),
                            Substitute("unable to parse PB: $0", body_buf.ToString()));
      break;
    }
    default: LOG(FATAL) << "Unknown mode";
  }

  VLOG(1) << "Received message: " << pb_util::SecureDebugString(*message);
  return Status::OK();
}

template <class M>
Status ControlShellProtocol::SendMessage(const M& message) {
  VLOG(1) << "Sending message: " << pb_util::SecureDebugString(message);

  faststring buf;
  switch (serialization_mode_) {
    case SerializationMode::JSON:
    {
      string serialized;
      const auto& google_status =
          google::protobuf::util::MessageToJsonString(message, &serialized);
      if (!google_status.ok()) {
        return Status::InvalidArgument(Substitute(
            "unable to serialize JSON: $0", pb_util::SecureDebugString(message)),
                                       google_status.error_message().ToString());
      }

      buf.append(serialized);
      buf.append("\n");
      break;
    }
    case SerializationMode::PB:
    {
      size_t msg_size = message.ByteSizeLong();
      buf.resize(sizeof(uint32_t) + msg_size);
      NetworkByteOrder::Store32(buf.data(), msg_size);
      if (!message.SerializeWithCachedSizesToArray(buf.data() + sizeof(uint32_t))) {
        return Status::Corruption("failed to serialize PB to array");
      }
      break;
    }
    default:
      break;
  }
  RETURN_NOT_OK_PREPEND(DoWrite(buf), "unable to send message");
  return Status::OK();
}

Status ControlShellProtocol::DoRead(faststring* buf) {
  uint8_t* pos = buf->data();
  size_t rem = buf->length();
  while (rem > 0) {
    ssize_t r;
    RETRY_ON_EINTR(r, read(read_fd_, pos, rem));
    if (r == -1) {
      return Status::IOError("Error reading from pipe", "", errno);
    }
    if (r == 0) {
      return Status::EndOfFile("Other end of pipe was closed");
    }
    DCHECK_GE(rem, r);
    rem -= r;
    pos += r;
  }
  return Status::OK();
}

Status ControlShellProtocol::DoWrite(const faststring& buf) {
  const uint8_t* pos = buf.data();
  size_t rem = buf.length();
  while (rem > 0) {
    ssize_t r;
    RETRY_ON_EINTR(r, write(write_fd_, pos, rem));
    if (r == -1) {
      if (errno == EPIPE) {
        return Status::EndOfFile("Other end of pipe was closed");
      }
      return Status::IOError("Error writing to pipe", "", errno);
    }
    DCHECK_GE(rem, r);
    rem -= r;
    pos += r;
  }
  return Status::OK();
}

// Explicit specialization for callers outside this compilation unit.
template
Status ControlShellProtocol::ReceiveMessage(ControlShellRequestPB* message);
template
Status ControlShellProtocol::ReceiveMessage(ControlShellResponsePB* message);
template
Status ControlShellProtocol::SendMessage(const ControlShellRequestPB& message);
template
Status ControlShellProtocol::SendMessage(const ControlShellResponsePB& message);

} // namespace tools
} // namespace kudu
