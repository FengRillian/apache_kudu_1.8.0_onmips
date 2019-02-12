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

#include "kudu/tools/tool_action.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <gflags/gflags_declare.h>
#include <glog/logging.h>

#include "kudu/common/common.pb.h"
#include "kudu/common/wire_protocol.h"
#include "kudu/common/wire_protocol.pb.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/split.h"
#include "kudu/gutil/strings/stringpiece.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/master/master.h"
#include "kudu/master/master.pb.h"
#include "kudu/master/master.proxy.h"
#include "kudu/tools/tool_action_common.h"
#include "kudu/util/status.h"

DECLARE_string(columns);

namespace kudu {

using master::ListMastersRequestPB;
using master::ListMastersResponsePB;
using master::MasterServiceProxy;
using std::cout;
using std::string;
using std::unique_ptr;
using std::vector;

namespace tools {
namespace {

const char* const kMasterAddressArg = "master_address";
const char* const kMasterAddressDesc = "Address of a Kudu Master of form "
    "'hostname:port'. Port may be omitted if the Master is bound to the "
    "default port.";
const char* const kFlagArg = "flag";
const char* const kValueArg = "value";

Status MasterGetFlags(const RunnerContext& context) {
  const string& address = FindOrDie(context.required_args, kMasterAddressArg);
  return PrintServerFlags(address, master::Master::kDefaultPort);
}

Status MasterSetFlag(const RunnerContext& context) {
  const string& address = FindOrDie(context.required_args, kMasterAddressArg);
  const string& flag = FindOrDie(context.required_args, kFlagArg);
  const string& value = FindOrDie(context.required_args, kValueArg);
  return SetServerFlag(address, master::Master::kDefaultPort, flag, value);
}

Status MasterStatus(const RunnerContext& context) {
  const string& address = FindOrDie(context.required_args, kMasterAddressArg);
  return PrintServerStatus(address, master::Master::kDefaultPort);
}

Status MasterTimestamp(const RunnerContext& context) {
  const string& address = FindOrDie(context.required_args, kMasterAddressArg);
  return PrintServerTimestamp(address, master::Master::kDefaultPort);
}

Status ListMasters(const RunnerContext& context) {
  LeaderMasterProxy proxy;
  RETURN_NOT_OK(proxy.Init(context));

  ListMastersRequestPB req;
  ListMastersResponsePB resp;

  proxy.SyncRpc<ListMastersRequestPB, ListMastersResponsePB>(
      req, &resp, "ListMasters", &MasterServiceProxy::ListMasters);

  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  DataTable table({});

  vector<ServerEntryPB> masters;
  std::copy_if(resp.masters().begin(), resp.masters().end(), std::back_inserter(masters),
               [](const ServerEntryPB& master) {
                 if (master.has_error()) {
                   LOG(WARNING) << "Failed to retrieve info for master: "
                                << StatusFromPB(master.error()).ToString();
                   return false;
                 }
                 return true;
               });

  auto hostport_to_string = [] (const HostPortPB& hostport) {
    return strings::Substitute("$0:$1", hostport.host(), hostport.port());
  };

  for (const auto& column : strings::Split(FLAGS_columns, ",", strings::SkipEmpty())) {
    vector<string> values;
    if (boost::iequals(column, "uuid")) {
      for (const auto& master : masters) {
        values.push_back(master.instance_id().permanent_uuid());
      }
    } else if (boost::iequals(column, "seqno")) {
      for (const auto& master : masters) {
        values.push_back(std::to_string(master.instance_id().instance_seqno()));
      }
    } else if (boost::iequals(column, "rpc-addresses") ||
               boost::iequals(column, "rpc_addresses")) {
      for (const auto& master : masters) {
        values.push_back(JoinMapped(master.registration().rpc_addresses(),
                         hostport_to_string, ","));
      }
    } else if (boost::iequals(column, "http-addresses") ||
               boost::iequals(column, "http_addresses")) {
      for (const auto& master : masters) {
        values.push_back(JoinMapped(master.registration().http_addresses(),
                                    hostport_to_string, ","));
      }
    } else if (boost::iequals(column, "version")) {
      for (const auto& master : masters) {
        values.push_back(master.registration().software_version());
      }
    } else {
      return Status::InvalidArgument("unknown column (--columns)", column);
    }
    table.AddColumn(column.ToString(), std::move(values));
  }

  RETURN_NOT_OK(table.PrintTo(cout));
  return Status::OK();
}

} // anonymous namespace

unique_ptr<Mode> BuildMasterMode() {
  unique_ptr<Action> get_flags =
      ActionBuilder("get_flags", &MasterGetFlags)
      .Description("Get the gflags for a Kudu Master")
      .AddRequiredParameter({ kMasterAddressArg, kMasterAddressDesc })
      .AddOptionalParameter("all_flags")
      .AddOptionalParameter("flag_tags")
      .Build();

  unique_ptr<Action> set_flag =
      ActionBuilder("set_flag", &MasterSetFlag)
      .Description("Change a gflag value on a Kudu Master")
      .AddRequiredParameter({ kMasterAddressArg, kMasterAddressDesc })
      .AddRequiredParameter({ kFlagArg, "Name of the gflag" })
      .AddRequiredParameter({ kValueArg, "New value for the gflag" })
      .AddOptionalParameter("force")
      .Build();

  unique_ptr<Action> status =
      ActionBuilder("status", &MasterStatus)
      .Description("Get the status of a Kudu Master")
      .AddRequiredParameter({ kMasterAddressArg, kMasterAddressDesc })
      .Build();

  unique_ptr<Action> timestamp =
      ActionBuilder("timestamp", &MasterTimestamp)
      .Description("Get the current timestamp of a Kudu Master")
      .AddRequiredParameter({ kMasterAddressArg, kMasterAddressDesc })
      .Build();

  unique_ptr<Action> list_masters =
      ActionBuilder("list", &ListMasters)
      .Description("List masters in a Kudu cluster")
      .AddRequiredParameter({ kMasterAddressesArg, kMasterAddressesArgDesc })
      .AddOptionalParameter("columns", string("uuid,rpc-addresses"),
                            string("Comma-separated list of master info fields to "
                                   "include in output.\nPossible values: uuid, "
                                   "rpc-addresses, http-addresses, version, and seqno"))
      .AddOptionalParameter("format")
      .AddOptionalParameter("timeout_ms")
      .Build();

  return ModeBuilder("master")
      .Description("Operate on a Kudu Master")
      .AddAction(std::move(get_flags))
      .AddAction(std::move(set_flag))
      .AddAction(std::move(status))
      .AddAction(std::move(timestamp))
      .AddAction(std::move(list_masters))
      .Build();
}

} // namespace tools
} // namespace kudu

