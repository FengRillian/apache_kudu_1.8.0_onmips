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

#include "kudu/sentry/mini_sentry.h"

#include <algorithm>
#include <csignal>
#include <map>
#include <memory>
#include <ostream>
#include <string>

#include <glog/logging.h>

#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/env.h"
#include "kudu/util/monotime.h"
#include "kudu/util/path_util.h"
#include "kudu/util/slice.h"
#include "kudu/util/status.h"
#include "kudu/util/stopwatch.h"
#include "kudu/util/subprocess.h"
#include "kudu/util/test_util.h"

using std::map;
using std::string;
using std::unique_ptr;
using strings::Substitute;

static constexpr int kSentryStartTimeoutMs = 60000;

namespace kudu {
namespace sentry {

MiniSentry::MiniSentry() {
}

MiniSentry::~MiniSentry() {
  WARN_NOT_OK(Stop(), "Failed to stop MiniSentry");
}

void MiniSentry::EnableKerberos(std::string krb5_conf,
                                std::string service_principal,
                                std::string keytab_file) {
  CHECK(!sentry_process_);
  CHECK(!krb5_conf.empty());
  CHECK(!service_principal.empty());
  CHECK(!keytab_file.empty());
  krb5_conf_ = std::move(krb5_conf);
  service_principal_ = std::move(service_principal);
  keytab_file_ = std::move(keytab_file);
}

Status MiniSentry::Start() {
  SCOPED_LOG_SLOW_EXECUTION(WARNING, kSentryStartTimeoutMs / 2, "Starting Sentry");
  CHECK(!sentry_process_);

  VLOG(1) << "Starting Sentry";

  Env* env = Env::Default();

  string exe;
  RETURN_NOT_OK(env->GetExecutablePath(&exe));
  const string bin_dir = DirName(exe);

  string hadoop_home;
  string sentry_home;
  string java_home;
  RETURN_NOT_OK(FindHomeDir("hadoop", bin_dir, &hadoop_home));
  RETURN_NOT_OK(FindHomeDir("sentry", bin_dir, &sentry_home));
  RETURN_NOT_OK(FindHomeDir("java", bin_dir, &java_home));

  auto tmp_dir = GetTestDataDirectory();

  RETURN_NOT_OK(CreateSentryConfigs(tmp_dir));

  // List of JVM environment options to pass to the Sentry service.
  string java_options;
  if (!krb5_conf_.empty()) {
    java_options += Substitute(" -Djava.security.krb5.conf=$0", krb5_conf_);
  }

  map<string, string> env_vars {
      { "JAVA_HOME", java_home },
      { "HADOOP_HOME", hadoop_home },
      { "JAVA_TOOL_OPTIONS", java_options },
  };

  // Start Sentry.
  sentry_process_.reset(new Subprocess({
      Substitute("$0/bin/sentry", sentry_home),
      "--command", "service",
      "--conffile", JoinPathSegments(tmp_dir, "sentry-site.xml"),
  }));

  sentry_process_->SetEnvVars(env_vars);
  RETURN_NOT_OK(sentry_process_->Start());

  // Wait for Sentry to start listening on its ports and commencing operation.
  VLOG(1) << "Waiting for Sentry ports";
  Status wait = WaitForTcpBind(sentry_process_->pid(), &port_,
                               MonoDelta::FromMilliseconds(kSentryStartTimeoutMs));
  if (!wait.ok()) {
    WARN_NOT_OK(sentry_process_->Kill(SIGQUIT), "failed to send SIGQUIT to Sentry");
  }
  return wait;
}

Status MiniSentry::Stop() {
  if (sentry_process_) {
    VLOG(1) << "Stopping Sentry";
    unique_ptr<Subprocess> proc = std::move(sentry_process_);
    RETURN_NOT_OK_PREPEND(proc->KillAndWait(SIGTERM), "failed to stop the Sentry service");
  }
  return Status::OK();
}

Status MiniSentry::Pause() {
  CHECK(sentry_process_);
  VLOG(1) << "Pausing Sentry";
  RETURN_NOT_OK_PREPEND(sentry_process_->Kill(SIGSTOP),
                        "failed to pause the Sentry service");
  return Status::OK();
}

Status MiniSentry::Resume() {
  CHECK(sentry_process_);
  VLOG(1) << "Resuming Sentry";
  RETURN_NOT_OK_PREPEND(sentry_process_->Kill(SIGCONT),
                        "failed to unpause the Sentry service");
  return Status::OK();
}

Status MiniSentry::CreateSentryConfigs(const string& tmp_dir) const {

  // - sentry.store.jdbc.url
  // - sentry.store.jdbc.password
  //     Configures Sentry to use a local in-process Derby instance with a dummy
  //     password value.
  //
  // - datanucleus.schema.autoCreateAll
  // - sentry.verify.schema.version
  //     Allow Sentry to startup and run without first running the schemaTool.
  //
  // - sentry.store.group.mapping
  //   sentry.store.group.mapping.resource
  //     Production Sentry instances use Hadoop's UGI infrastructure to map users
  //     to groups, but that's difficult to mock for tests, so we configure a
  //     simpler static user/group mapping based on a generated INI file.
  //
  // - sentry.service.admin.group
  //     Set up admin groups which have unrestricted privileges in Sentry.
  //
  // - sentry.service.allow.connect
  //     Set of Kerberos principals which is allowed to connect to Sentry when
  //     the Kerberos security mode is enabled.
  static const string kFileTemplate = R"(
<configuration>

  <property>
    <name>sentry.service.security.mode</name>
    <value>$0</value>
  </property>

  <property>
    <name>sentry.service.server.principal</name>
    <value>$1</value>
  </property>

  <property>
    <name>sentry.service.server.keytab</name>
    <value>$2</value>
  </property>

  <property>
    <name>sentry.store.jdbc.url</name>
    <value>jdbc:derby:$3/sentry;create=true</value>
  </property>

  <property>
    <name>sentry.store.jdbc.password</name>
    <value>_</value>
  </property>

  <property>
    <name>datanucleus.schema.autoCreateAll</name>
    <value>true</value>
  </property>

  <property>
    <name>sentry.verify.schema.version</name>
    <value>false</value>
  </property>

  <property>
    <name>sentry.store.group.mapping</name>
    <value>org.apache.sentry.provider.file.LocalGroupMappingService</value>
  </property>

  <property>
    <name>sentry.store.group.mapping.resource</name>
    <value>$4</value>
  </property>

  <property>
    <name>sentry.service.admin.group</name>
    <value>admin</value>
  </property>

  <property>
    <name>sentry.service.allow.connect</name>
    <value>kudu</value>
  </property>

</configuration>
  )";

  string users_ini_path = JoinPathSegments(tmp_dir, "users.ini");
  string file_contents = Substitute(
      kFileTemplate,
      keytab_file_.empty() ? "none" : "kerberos",
      service_principal_,
      keytab_file_,
      tmp_dir,
      users_ini_path);
  RETURN_NOT_OK(WriteStringToFile(Env::Default(),
                                  file_contents,
                                  JoinPathSegments(tmp_dir, "sentry-site.xml")));

  // Simple file format containing mapping of user to groups in INI syntax, see
  // the LocalGroupMappingService class for more information.
  static const string kUsers = R"(
[users]
test-admin=admin
test-user=user
kudu=admin
joe-interloper=""
)";

  RETURN_NOT_OK(WriteStringToFile(Env::Default(), kUsers, users_ini_path));
  return Status::OK();
}

} // namespace sentry
} // namespace kudu
