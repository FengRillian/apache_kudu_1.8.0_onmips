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

#include "kudu/hms/hms_catalog.h"

#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "kudu/common/common.pb.h"
#include "kudu/common/schema.h"
#include "kudu/common/types.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/strings/ascii_ctype.h"
#include "kudu/gutil/strings/charset.h"
#include "kudu/gutil/strings/split.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/hms/hive_metastore_types.h"
#include "kudu/hms/hms_client.h"
#include "kudu/thrift/client.h"
#include "kudu/util/flag_tags.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/slice.h"

using boost::none;
using boost::optional;
using std::move;
using std::string;
using std::vector;
using strings::Substitute;

DEFINE_string(hive_metastore_uris, "",
              "Address of the Hive Metastore instance(s). The provided port must be for the HMS "
              "Thrift service. If a port is not provided, defaults to 9083. If the HMS is deployed "
              "in an HA configuration, multiple comma-separated addresses should be supplied. "
              "If not set, the Kudu master will not send Kudu table catalog updates to Hive. The "
              "configured value must match the Hive hive.metastore.uris configuration.");
DEFINE_validator(hive_metastore_uris, &kudu::hms::HmsCatalog::ValidateUris);
TAG_FLAG(hive_metastore_uris, experimental);

// Note: the hive_metastore_sasl_enabled and keytab_file combination is validated in master.cc.
DEFINE_bool(hive_metastore_sasl_enabled, false,
            "Configures whether Thrift connections to the Hive Metastore use SASL "
            "(Kerberos) security. Must match the value of the "
            "hive.metastore.sasl.enabled option in the Hive Metastore configuration. "
            "When enabled, the --keytab_file flag must be provided.");
TAG_FLAG(hive_metastore_sasl_enabled, experimental);

DEFINE_string(hive_metastore_kerberos_principal, "hive",
              "The service principal of the Hive Metastore server. Must match "
              "the primary (user) portion of hive.metastore.kerberos.principal option "
              "in the Hive Metastore configuration.");
TAG_FLAG(hive_metastore_kerberos_principal, experimental);

DEFINE_int32(hive_metastore_retry_count, 1,
             "The number of times that HMS operations will retry after "
             "encountering retriable failures, such as network errors.");
TAG_FLAG(hive_metastore_retry_count, advanced);
TAG_FLAG(hive_metastore_retry_count, experimental);
TAG_FLAG(hive_metastore_retry_count, runtime);

DEFINE_int32(hive_metastore_send_timeout, 60,
             "Configures the socket send timeout, in seconds, for Thrift "
             "connections to the Hive Metastore.");
TAG_FLAG(hive_metastore_send_timeout, advanced);
TAG_FLAG(hive_metastore_send_timeout, experimental);
TAG_FLAG(hive_metastore_send_timeout, runtime);

DEFINE_int32(hive_metastore_recv_timeout, 60,
             "Configures the socket receive timeout, in seconds, for Thrift "
             "connections to the Hive Metastore.");
TAG_FLAG(hive_metastore_recv_timeout, advanced);
TAG_FLAG(hive_metastore_recv_timeout, experimental);
TAG_FLAG(hive_metastore_recv_timeout, runtime);

DEFINE_int32(hive_metastore_conn_timeout, 60,
             "Configures the socket connect timeout, in seconds, for Thrift "
             "connections to the Hive Metastore.");
TAG_FLAG(hive_metastore_conn_timeout, advanced);
TAG_FLAG(hive_metastore_conn_timeout, experimental);
TAG_FLAG(hive_metastore_conn_timeout, runtime);

DEFINE_int32(hive_metastore_max_message_size, 100 * 1024 * 1024,
             "Maximum size of Hive Metastore objects that can be received by the "
             "HMS client in bytes. Should match the metastore.server.max.message.size "
             "configuration.");
TAG_FLAG(hive_metastore_max_message_size, advanced);
TAG_FLAG(hive_metastore_max_message_size, experimental);
TAG_FLAG(hive_metastore_max_message_size, runtime);

namespace kudu {
namespace hms {

const char* const HmsCatalog::kInvalidTableError = "when the Hive Metastore integration "
    "is enabled, Kudu table names must be a period ('.') separated database and table name "
    "identifier pair, each containing only ASCII alphanumeric characters, '_', and '/'";

HmsCatalog::HmsCatalog(string master_addresses)
    : master_addresses_(std::move(master_addresses)) {
}

HmsCatalog::~HmsCatalog() {
  Stop();
}

Status HmsCatalog::Start() {
  vector<HostPort> addresses;
  RETURN_NOT_OK(ParseUris(FLAGS_hive_metastore_uris, &addresses));

  thrift::ClientOptions options;
  options.send_timeout = MonoDelta::FromSeconds(FLAGS_hive_metastore_send_timeout);
  options.recv_timeout = MonoDelta::FromSeconds(FLAGS_hive_metastore_recv_timeout);
  options.conn_timeout = MonoDelta::FromSeconds(FLAGS_hive_metastore_conn_timeout);
  options.enable_kerberos = FLAGS_hive_metastore_sasl_enabled;
  options.service_principal = FLAGS_hive_metastore_kerberos_principal;
  options.max_buf_size = FLAGS_hive_metastore_max_message_size;
  options.retry_count = FLAGS_hive_metastore_retry_count;

  return ha_client_.Start(std::move(addresses), std::move(options));
}

void HmsCatalog::Stop() {
  ha_client_.Stop();
}

Status HmsCatalog::CreateTable(const string& id,
                               const string& name,
                               optional<const string&> owner,
                               const Schema& schema) {
  hive::Table table;
  RETURN_NOT_OK(PopulateTable(id, name, owner, schema, master_addresses_, &table));
  return ha_client_.Execute([&] (HmsClient* client) {
      return client->CreateTable(table, EnvironmentContext());
  });
}

Status HmsCatalog::DropTable(const string& id, const string& name) {
  hive::EnvironmentContext env_ctx = EnvironmentContext();
  env_ctx.properties.insert(make_pair(HmsClient::kKuduTableIdKey, id));
  return DropTable(name, env_ctx);
}

Status HmsCatalog::DropLegacyTable(const string& name) {
  return DropTable(name, EnvironmentContext());
}

Status HmsCatalog::DropTable(const string& name, const hive::EnvironmentContext& env_ctx) {
  Slice hms_database;
  Slice hms_table;
  RETURN_NOT_OK(ParseTableName(name, &hms_database, &hms_table));
  return ha_client_.Execute([&] (HmsClient* client) {
    return client->DropTable(hms_database.ToString(), hms_table.ToString(), env_ctx);
  });
}

Status HmsCatalog::UpgradeLegacyImpalaTable(const string& id,
                                            const string& db_name,
                                            const string& tb_name,
                                            const Schema& schema) {
  return ha_client_.Execute([&] (HmsClient* client) {
    hive::Table table;
    RETURN_NOT_OK(client->GetTable(db_name, tb_name, &table));
    if (table.parameters[HmsClient::kStorageHandlerKey] !=
        HmsClient::kLegacyKuduStorageHandler) {
      return Status::IllegalState("non-legacy table cannot be upgraded");
    }

    RETURN_NOT_OK(PopulateTable(id, Substitute("$0.$1", db_name, tb_name),
                                none, schema, master_addresses_, &table));
    return client->AlterTable(db_name, tb_name, table, EnvironmentContext());
  });
}

Status HmsCatalog::DowngradeToLegacyImpalaTable(const string& name) {
  Slice hms_database;
  Slice hms_table;
  RETURN_NOT_OK(ParseTableName(name, &hms_database, &hms_table));

  return ha_client_.Execute([&] (HmsClient* client) {
    hive::Table table;
    RETURN_NOT_OK(client->GetTable(hms_database.ToString(), hms_table.ToString(), &table));
    if (table.parameters[HmsClient::kStorageHandlerKey] !=
        HmsClient::kKuduStorageHandler) {
      return Status::IllegalState("non-Kudu table cannot be downgraded");
    }

    // Add the legacy Kudu-specific parameters. And set it to
    // external table type.
    table.tableType = HmsClient::kExternalTable;
    table.parameters[HmsClient::kLegacyKuduTableNameKey] = name;
    table.parameters[HmsClient::kKuduMasterAddrsKey] = master_addresses_;
    table.parameters[HmsClient::kStorageHandlerKey] = HmsClient::kLegacyKuduStorageHandler;
    table.parameters[HmsClient::kExternalTableKey] = "TRUE";

    // Remove the Kudu-specific field 'kudu.table_id'.
    EraseKeyReturnValuePtr(&table.parameters, HmsClient::kKuduTableIdKey);

    return client->AlterTable(table.dbName, table.tableName, table, EnvironmentContext());
  });
}

Status HmsCatalog::GetKuduTables(vector<hive::Table>* kudu_tables) {
  return ha_client_.Execute([&] (HmsClient* client) {
    vector<string> database_names;
    RETURN_NOT_OK(client->GetAllDatabases(&database_names));
    vector<string> table_names;
    vector<hive::Table> tables;

    for (const auto& database_name : database_names) {
      table_names.clear();
      tables.clear();
      RETURN_NOT_OK(client->GetTableNames(
            database_name,
            Substitute("$0$1 = \"$2\" OR $0$1 = \"$3\"",
              HmsClient::kHiveFilterFieldParams,
              HmsClient::kStorageHandlerKey,
              HmsClient::kKuduStorageHandler,
              HmsClient::kLegacyKuduStorageHandler),
            &table_names));

      if (!table_names.empty()) {
        RETURN_NOT_OK(client->GetTables(database_name, table_names, &tables));
        std::move(tables.begin(), tables.end(), std::back_inserter(*kudu_tables));
      }
    }

    return Status::OK();
  });
}

Status HmsCatalog::AlterTable(const string& id,
                              const string& name,
                              const string& new_name,
                              const Schema& schema) {
  Slice hms_database;
  Slice hms_table;
  RETURN_NOT_OK(ParseTableName(name, &hms_database, &hms_table));

  return ha_client_.Execute([&] (HmsClient* client) {
      // The HMS does not have a way to alter individual fields of a table
      // entry, so we must request the existing table entry from the HMS, update
      // the fields, and write it back. Otherwise we'd overwrite metadata fields
      // that other tools put into place, such as table statistics. We do
      // overwrite all Kudu-specific entries such as the Kudu master addresses
      // and the full set of columns. This ensures entries are fully 'repaired'
      // during an alter operation.
      //
      // This can go wrong in a number of ways, including:
      //
      // - The original table name isn't a valid Hive database/table pair
      // - The new table name isn't a valid Hive database/table pair
      // - The original table does not exist in the HMS
      // - The original table doesn't match the Kudu table being altered

      hive::Table table;
      RETURN_NOT_OK(client->GetTable(hms_database.ToString(), hms_table.ToString(), &table));

      // Check that the HMS entry belongs to the table being altered.
      if (table.parameters[HmsClient::kStorageHandlerKey] != HmsClient::kKuduStorageHandler ||
          table.parameters[HmsClient::kKuduTableIdKey] != id) {
        // The original table isn't a Kudu table, or isn't the same Kudu table.
        return Status::NotFound("the HMS entry for the table being "
                                "altered belongs to another table");
      }

      // Overwrite fields in the table that have changed, including the new name.
      RETURN_NOT_OK(PopulateTable(id, new_name, none, schema, master_addresses_, &table));
      return client->AlterTable(hms_database.ToString(), hms_table.ToString(),
                                table, EnvironmentContext());
  });
}

Status HmsCatalog::GetNotificationEvents(int64_t last_event_id, int max_events,
                                         vector<hive::NotificationEvent>* events) {
  return ha_client_.Execute([&] (HmsClient* client) {
    return client->GetNotificationEvents(last_event_id, max_events, events);
  });
}

namespace {

string column_to_field_type(const ColumnSchema& column) {
  // See org.apache.hadoop.hive.serde.serdeConstants.
  switch (column.type_info()->type()) {
    case BOOL: return "boolean";
    case INT8: return "tinyint";
    case INT16: return "smallint";
    case INT32: return "int";
    case INT64: return "bigint";
    case DECIMAL32:
    case DECIMAL64:
    case DECIMAL128: return Substitute("decimal($0,$1)",
                                       column.type_attributes().precision,
                                       column.type_attributes().scale);
    case FLOAT: return "float";
    case DOUBLE: return "double";
    case STRING: return "string";
    case BINARY: return "binary";
    case UNIXTIME_MICROS: return "timestamp";
    default: LOG(FATAL) << "unhandled column type: " << column.TypeToString();
  }
  __builtin_unreachable();
}

hive::FieldSchema column_to_field(const ColumnSchema& column) {
  hive::FieldSchema field;
  field.name = column.name();
  field.type = column_to_field_type(column);
  return field;
}

// Convert an ASCII encoded string to lowercase in place.
void ToLowerCase(Slice s) {
  for (int i = 0; i < s.size(); i++) {
    s.mutable_data()[i] = ascii_tolower(s[i]);
  }
}
} // anonymous namespace

Status HmsCatalog::PopulateTable(const string& id,
                                 const string& name,
                                 const optional<const string&>& owner,
                                 const Schema& schema,
                                 const string& master_addresses,
                                 hive::Table* table) {
  Slice hms_database_name;
  Slice hms_table_name;
  RETURN_NOT_OK(ParseTableName(name, &hms_database_name, &hms_table_name));
  table->dbName = hms_database_name.ToString();
  table->tableName = hms_table_name.ToString();
  if (owner) {
    table->owner = *owner;
  }

  // Add the Kudu-specific parameters. This intentionally avoids overwriting
  // other parameters.
  table->parameters[HmsClient::kKuduTableIdKey] = id;
  table->parameters[HmsClient::kKuduMasterAddrsKey] = master_addresses;
  table->parameters[HmsClient::kStorageHandlerKey] = HmsClient::kKuduStorageHandler;
  // Workaround for HIVE-19253.
  table->parameters[HmsClient::kExternalTableKey] = "TRUE";

  // Set the table type to external so that the table's (HD)FS directory will
  // not be deleted when the table is dropped. Deleting the directory is
  // unnecessary, and causes a race in the HMS between concurrent DROP TABLE and
  // CREATE TABLE operations on existing tables.
  table->tableType = HmsClient::kExternalTable;

  // Remove the deprecated Kudu-specific field 'kudu.table_name'.
  EraseKeyReturnValuePtr(&table->parameters, HmsClient::kLegacyKuduTableNameKey);

  // Overwrite the entire set of columns.
  vector<hive::FieldSchema> fields;
  for (const auto& column : schema.columns()) {
    fields.emplace_back(column_to_field(column));
  }
  table->sd.cols = std::move(fields);

  return Status::OK();
}

Status HmsCatalog::NormalizeTableName(string* table_name) {
  CHECK_NOTNULL(table_name);
  Slice hms_database;
  Slice hms_table;
  RETURN_NOT_OK(ParseTableName(*table_name, &hms_database, &hms_table));

  ToLowerCase(hms_database);
  ToLowerCase(hms_table);

  return Status::OK();
}

Status HmsCatalog::ParseTableName(const string& table_name,
                                  Slice* hms_database,
                                  Slice* hms_table) {
  const char kSeparator = '.';
  strings::CharSet charset("abcdefghijklmnopqrstuvwxyz"
                           "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                           "0123456789"
                           "_/");

  optional<int> separator_idx;
  for (int idx = 0; idx < table_name.size(); idx++) {
    char c = table_name[idx];
    if (!charset.Test(c)) {
      if (c == kSeparator && !separator_idx) {
        separator_idx = idx;
      } else {
        return Status::InvalidArgument(kInvalidTableError, table_name);
      }
    }
  }
  if (!separator_idx || *separator_idx == 0 || *separator_idx == table_name.size() - 1) {
    return Status::InvalidArgument(kInvalidTableError, table_name);
  }

  *hms_database = Slice(table_name.data(), *separator_idx);
  *hms_table = Slice(table_name.data() + *separator_idx + 1,
                     table_name.size() - *separator_idx - 1);
  return Status::OK();
}

Status HmsCatalog::ParseUris(const string& metastore_uris, vector<HostPort>* hostports) {
  hostports->clear();

  vector<string> uris = strings::Split(metastore_uris, ",", strings::SkipEmpty());
  const string kSchemeSeparator = "://";

  for (auto& uri : uris) {
    auto scheme_idx = uri.find(kSchemeSeparator, 1);
    if (scheme_idx == string::npos) {
      return Status::InvalidArgument("invalid Hive Metastore URI: missing scheme", uri);
    }
    uri.erase(0, scheme_idx + kSchemeSeparator.size());

    HostPort hp;
    RETURN_NOT_OK(hp.ParseString(uri, HmsClient::kDefaultHmsPort));
    // Note: the Java HMS client canonicalizes the hostname to a FQDN at this
    // point. We skip that because the krb5 library should handle it for us
    // (when rdns = true), whereas the Java GSSAPI implementation apparently
    // never canonicalizes.
    //
    // See org.apache.hadoop.hive.metastore.HiveMetastoreClient.resolveUris.
    hostports->emplace_back(std::move(hp));
  }

  return Status::OK();
}

// Validates the hive_metastore_uris gflag.
bool HmsCatalog::ValidateUris(const char* flag_name, const string& metastore_uris) {
  vector<HostPort> host_ports;
  Status s = HmsCatalog::ParseUris(metastore_uris, &host_ports);
  if (!s.ok()) {
    LOG(ERROR) << "invalid flag " << flag_name << ": " << s.ToString();
  }
  return s.ok();
}

bool HmsCatalog::IsEnabled() {
  return !FLAGS_hive_metastore_uris.empty();
}

hive::EnvironmentContext HmsCatalog::EnvironmentContext() {
  hive::EnvironmentContext env_ctx;
  env_ctx.__set_properties({ std::make_pair(hms::HmsClient::kKuduMasterEventKey, "true") });
  return env_ctx;
}

} // namespace hms
} // namespace kudu
