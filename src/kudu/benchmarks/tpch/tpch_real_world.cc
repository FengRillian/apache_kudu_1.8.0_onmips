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
//
// Benchmarking tool to run tpch1 concurrently with inserts.
//
// Requirements:
//  - TPC-H's dbgen tool, compiled.
//  - Optionally, a running cluster. By default it starts its own external cluster.
//
// This tool has three main configurations:
//  - tpch_test_runtime_sec: the longest this test can run for, in seconds, excluding startup time.
//    By default, it runs until there's no more rows to insert.
//  - tpch_scaling_factor: the dbgen scaling factor to generate the data. The test will end if
//    dbgen is done generating data, even if there's still time left. The default is 1.
//  - tpch_path_to_dbgen: where to find dbgen, by default it assumes it can be found in the
//    current directory.
//
// This tool has three threads:
//  - One that runs "dbgen -T L" with the configured scale factor.
//  - One that reads from the "lineitem.tbl" named pipe and inserts the rows.
//  - One that runs tpch1 continuously and outputs the timings. This thread won't start until at
//    least some rows have been written, because dbgen takes some seconds to startup. It also
//    stops at soon as it gets the signal that we ran out of time or that there are no more rows to
//    insert, so the last timing shouldn't be used.
//
// TODO(KUDU-629) Make the inserts multi-threaded.

#include <sys/stat.h>

#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "kudu/benchmarks/tpch/line_item_tsv_importer.h"
#include "kudu/benchmarks/tpch/rpc_line_item_dao.h"
#include "kudu/benchmarks/tpch/tpch-schemas.h"
#include "kudu/client/row_result.h"
#include "kudu/client/schema.h"
#include "kudu/common/partial_row.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/stl_util.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/mini-cluster/external_mini_cluster.h"
#include "kudu/util/atomic.h"
#include "kudu/util/countdown_latch.h"
#include "kudu/util/env.h"
#include "kudu/util/errno.h"
#include "kudu/util/flags.h"
#include "kudu/util/logging.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/status.h"
#include "kudu/util/stopwatch.h"
#include "kudu/util/subprocess.h"
#include "kudu/util/thread.h"

DEFINE_bool(tpch_use_mini_cluster, true,
            "Create a mini cluster for the work to be performed against");
DEFINE_bool(tpch_load_data, true,
            "Load dbgen data");
DEFINE_bool(tpch_run_queries, true,
            "Query dbgen data as it is inserted");
DEFINE_int32(tpch_max_batch_size, 1000,
             "Maximum number of inserts/updates to batch at once.  Set to 0 "
             "to delegate the batching control to the logic of the "
             "KuduSession running in AUTO_BACKGROUND_MODE flush mode.");
DEFINE_int32(tpch_test_client_timeout_msec, 10000,
             "Timeout that will be used for all operations and RPCs");
DEFINE_int32(tpch_test_runtime_sec, 0,
             "How long this test should run for excluding startup time (note that it will also "
             "stop if dbgen finished generating all its data)");
DEFINE_int32(tpch_scaling_factor, 1,
             "Scaling factor to use with dbgen, the default is 1");
DEFINE_int32(tpch_num_inserters, 1,
             "Number of data inserters to run in parallel. Each inserter implies a new tablet "
             "in the table");
DEFINE_string(tpch_master_addresses, "localhost",
              "Addresses of masters for the cluster to operate on if not using a mini cluster");
DEFINE_string(tpch_mini_cluster_base_dir, "/tmp/tpch",
              "If using a mini cluster, directory for master/ts data");
DEFINE_string(tpch_path_to_dbgen_dir, ".",
              "Path to the directory where the dbgen executable can be found");
DEFINE_string(tpch_path_to_ts_flags_file, "",
              "Path to the file that contains extra flags for the tablet servers if using "
              "a mini cluster. Doesn't use one by default.");
DEFINE_string(tpch_table_name, "tpch_real_world",
              "Table name to use during the test");
DEFINE_string(tpch_partition_strategy, "range",
              "The partitioning strategy to use for the lineitem table. If 'range' is selected, "
              "each writer thread inserts sequentially into its own tablet, creating an ideal "
              "workload for maximum throughput. 'hash' partitioning creates the same number of "
              "tablets, but hash-partitions them so that each writer thread writes to all "
              "tablets. This is less ideal, but more faithfully represents a lot of write "
              "workloads.");

using std::string;
using std::vector;

namespace kudu {

using client::KuduRowResult;
using client::KuduSchema;
using cluster::ExternalMiniCluster;
using cluster::ExternalMiniClusterOptions;
using strings::Substitute;

class TpchRealWorld {
 public:
  TpchRealWorld()
    : rows_inserted_(0),
      stop_threads_(false),
      dbgen_processes_finished_(FLAGS_tpch_num_inserters) {
  }

  ~TpchRealWorld() {
    STLDeleteElements(&dbgen_processes_);
  }

  Status Init();

  gscoped_ptr<RpcLineItemDAO> GetInittedDAO();

  void LoadLineItemsThread(int i);

  void MonitorDbgenThread(int i);

  void RunQueriesThread();

  void WaitForRowCount(int64_t row_count);

  Status Run();

 private:
  static const char* kLineItemBase;

  Status CreateFifos();
  Status StartDbgens();
  string GetNthLineItemFileName(int i) const {
    // This is dbgen's naming convention; we're just following it.
    return FLAGS_tpch_num_inserters > 1
        ? Substitute("$0.$1", kLineItemBase, i + 1) : kLineItemBase;
  }

  gscoped_ptr<ExternalMiniCluster> cluster_;
  AtomicInt<int64_t> rows_inserted_;
  string master_addresses_;
  AtomicBool stop_threads_;
  CountDownLatch dbgen_processes_finished_;

  vector<Subprocess*> dbgen_processes_;
};

const char* TpchRealWorld::kLineItemBase = "lineitem.tbl";

Status TpchRealWorld::Init() {
  Env* env = Env::Default();
  if (FLAGS_tpch_use_mini_cluster) {
    if (FLAGS_tpch_load_data) {
      if (env->FileExists(FLAGS_tpch_mini_cluster_base_dir)) {
        RETURN_NOT_OK(env->DeleteRecursively(FLAGS_tpch_mini_cluster_base_dir));
      }
      RETURN_NOT_OK(env->CreateDir(FLAGS_tpch_mini_cluster_base_dir));
    }

    ExternalMiniClusterOptions opts;
    opts.num_tablet_servers = 1;
    opts.cluster_root = FLAGS_tpch_mini_cluster_base_dir;
    if (!FLAGS_tpch_path_to_ts_flags_file.empty()) {
      opts.extra_tserver_flags.push_back("--flagfile=" + FLAGS_tpch_path_to_ts_flags_file);
    }

    cluster_.reset(new ExternalMiniCluster(std::move(opts)));
    RETURN_NOT_OK(cluster_->Start());
    master_addresses_ = cluster_->leader_master()->bound_rpc_hostport().ToString();
  } else {
    master_addresses_ = FLAGS_tpch_master_addresses;
  }

  // Create the table before any other DAOs are constructed.
  GetInittedDAO();

  if (FLAGS_tpch_load_data) {
    RETURN_NOT_OK(CreateFifos());
    RETURN_NOT_OK(StartDbgens());
  }

  return Status::OK();
}

Status TpchRealWorld::CreateFifos() {
  for (int i = 0; i < FLAGS_tpch_num_inserters; i++) {
    string path_to_lineitem = GetNthLineItemFileName(i);
    struct stat sbuf;
    if (stat(path_to_lineitem.c_str(), &sbuf) != 0) {
      if (errno == ENOENT) {
        if (mkfifo(path_to_lineitem.c_str(), 0644) != 0) {
          string msg = Substitute("Could not create the named pipe for the dbgen output at $0",
                                  path_to_lineitem);
          return Status::InvalidArgument(msg);
        }
      } else {
        return Status::IOError(path_to_lineitem, ErrnoToString(errno), errno);
      }
    } else {
      if (!S_ISFIFO(sbuf.st_mode)) {
        string msg = Substitute("Please remove the current lineitem file at $0",
                                path_to_lineitem);
        return Status::InvalidArgument(msg);
      }
    }
    // We get here if the file was already a fifo or if we created it.
  }
  return Status::OK();
}

Status TpchRealWorld::StartDbgens() {
  for (int i = 1; i <= FLAGS_tpch_num_inserters; i++) {
    // This environment variable is necessary if dbgen isn't in the current dir.
    setenv("DSS_CONFIG", FLAGS_tpch_path_to_dbgen_dir.c_str(), 1);
    vector<string> argv;
    argv.push_back(Substitute("$0/dbgen", FLAGS_tpch_path_to_dbgen_dir));
    argv.emplace_back("-q");
    argv.emplace_back("-T");
    argv.emplace_back("L");
    argv.emplace_back("-s");
    argv.push_back(Substitute("$0", FLAGS_tpch_scaling_factor));
    if (FLAGS_tpch_num_inserters > 1) {
      argv.emplace_back("-C");
      argv.push_back(Substitute("$0", FLAGS_tpch_num_inserters));
      argv.emplace_back("-S");
      argv.push_back(Substitute("$0", i));
    }
    gscoped_ptr<Subprocess> dbgen_proc(new Subprocess(argv));
    LOG(INFO) << "Running " << JoinStrings(argv, " ");
    RETURN_NOT_OK(dbgen_proc->Start());
    dbgen_processes_.push_back(dbgen_proc.release());
  }
  return Status::OK();
}

gscoped_ptr<RpcLineItemDAO> TpchRealWorld::GetInittedDAO() {
  // When chunking, dbgen will begin the nth chunk on the order key:
  //
  //   6000000 * SF * n / num_chunks
  //
  // For example, when run with SF=2 and three chunks, the first keys for each
  // chunk are 1, 4000001, and 8000001.
  int64_t increment = 6000000L * FLAGS_tpch_scaling_factor /
      FLAGS_tpch_num_inserters;

  KuduSchema schema(tpch::CreateLineItemSchema());
  vector<const KuduPartialRow*> split_rows;

  RpcLineItemDAO::PartitionStrategy strategy;
  if (FLAGS_tpch_partition_strategy == "hash") {
    strategy = RpcLineItemDAO::HASH;
  } else if (FLAGS_tpch_partition_strategy == "range") {
    strategy = RpcLineItemDAO::RANGE;
    for (int64_t i = 1; i < FLAGS_tpch_num_inserters; i++) {
      KuduPartialRow* row = schema.NewRow();
      CHECK_OK(row->SetInt64(tpch::kOrderKeyColName, i * increment));
      CHECK_OK(row->SetInt32(tpch::kLineNumberColName, 0));
      split_rows.push_back(row);
    }
  } else {
    LOG(FATAL) << "Unknown partition strategy: " << FLAGS_tpch_partition_strategy;
  }

  gscoped_ptr<RpcLineItemDAO> dao(
        new RpcLineItemDAO(master_addresses_,
                           FLAGS_tpch_table_name,
                           FLAGS_tpch_max_batch_size,
                           FLAGS_tpch_test_client_timeout_msec,
                           strategy,
                           FLAGS_tpch_num_inserters,
                           split_rows));
  dao->Init();
  return std::move(dao);
}

void TpchRealWorld::LoadLineItemsThread(int i) {
  LOG(INFO) << "Connecting to cluster at " << master_addresses_;
  gscoped_ptr<RpcLineItemDAO> dao = GetInittedDAO();
  LineItemTsvImporter importer(GetNthLineItemFileName(i));

  boost::function<void(KuduPartialRow*)> f =
      boost::bind(&LineItemTsvImporter::GetNextLine, &importer, _1);
  const string time_spent_msg = Substitute(
        "by thread $0 to load generated data into the database", i);
  LOG_TIMING(INFO, time_spent_msg) {
    while (importer.HasNextLine() && !stop_threads_.Load()) {
      dao->WriteLine(f);
      int64_t current_count = rows_inserted_.Increment();
      if (current_count % 250000 == 0) {
        LOG(INFO) << "Inserted " << current_count << " rows";
      }
    }
    dao->FinishWriting();
  }
  LOG(INFO) << Substitute("Thread $0 inserted ", i)
            << rows_inserted_.Load() << " rows in total";
}

void TpchRealWorld::MonitorDbgenThread(int i) {
  Subprocess* dbgen_proc = dbgen_processes_[i];
  while (!stop_threads_.Load()) {
    Status s = dbgen_proc->WaitNoBlock();
    if (s.ok()) {
      int exit_status;
      string exit_info;
      CHECK_OK(dbgen_proc->GetExitStatus(&exit_status, &exit_info));
      if (exit_status != 0) {
        LOG(FATAL) << exit_info;
      }
      LOG(INFO) << "dbgen finished inserting data";
      dbgen_processes_finished_.CountDown();
      return;
    }
    CHECK(s.IsTimedOut()) << "Unexpected wait status: " << s.ToString();
    SleepFor(MonoDelta::FromMilliseconds(100));
  }
  Status s = dbgen_proc->Kill(SIGKILL);
  if (!s.ok()) {
    LOG(FATAL) << "Failed to send SIGKILL to dbgen: " << s.ToString();
  }
  s = dbgen_proc->Wait();
  if (!s.ok()) {
    LOG(FATAL) << "Failed to await for dbgen exit: " << s.ToString();
  }
}

void TpchRealWorld::RunQueriesThread() {
  gscoped_ptr<RpcLineItemDAO> dao = GetInittedDAO();
  while (!stop_threads_.Load()) {
    string log;
    if (FLAGS_tpch_load_data) {
      log = StringPrintf("querying %" PRId64 " rows", rows_inserted_.Load());
    } else {
      log = "querying data in cluster";
    }
    LOG_TIMING(INFO, log) {
      gscoped_ptr<RpcLineItemDAO::Scanner> scanner;
      dao->OpenTpch1Scanner(&scanner);
      vector<KuduRowResult> rows;
      // We check stop_threads_ even while scanning since it can takes tens of seconds to query.
      // This means that the last timing cannot be used for reporting.
      while (scanner->HasMore() && !stop_threads_.Load()) {
        scanner->GetNext(&rows);
      }
    }
  }
}

void TpchRealWorld::WaitForRowCount(int64_t row_count) {
  while (rows_inserted_.Load() < row_count) {
    SleepFor(MonoDelta::FromMilliseconds(100));
  }
}

Status TpchRealWorld::Run() {
  vector<scoped_refptr<Thread> > threads;
  if (FLAGS_tpch_load_data) {
    for (int i = 0; i < FLAGS_tpch_num_inserters; i++) {
      scoped_refptr<kudu::Thread> thr;
      RETURN_NOT_OK(kudu::Thread::Create("test", Substitute("lineitem-gen$0", i),
                                         &TpchRealWorld::MonitorDbgenThread, this, i,
                                         &thr));
      threads.push_back(thr);
      RETURN_NOT_OK(kudu::Thread::Create("test", Substitute("lineitem-load$0", i),
                                         &TpchRealWorld::LoadLineItemsThread, this, i,
                                         &thr));
      threads.push_back(thr);
    }

    // It takes some time for dbgen to start outputting rows so there's no need to query yet.
    LOG(INFO) << "Waiting for dbgen to start...";
    WaitForRowCount(10000);
  }

  if (FLAGS_tpch_run_queries) {
    scoped_refptr<kudu::Thread> thr;
    RETURN_NOT_OK(kudu::Thread::Create("test", "lineitem-query",
                                       &TpchRealWorld::RunQueriesThread, this,
                                       &thr));
    threads.push_back(thr);
  }

  // We'll wait until all the dbgens finish or after tpch_test_runtime_sec,
  // whichever comes first.
  if (FLAGS_tpch_test_runtime_sec > 0) {
    if (!dbgen_processes_finished_.WaitFor(
        MonoDelta::FromSeconds(FLAGS_tpch_test_runtime_sec))) {
      LOG(WARNING) << FLAGS_tpch_test_runtime_sec
                   << " seconds expired, killing test";
    }
  } else {
    dbgen_processes_finished_.Wait();
  }

  if (!FLAGS_tpch_load_data) {
    SleepFor(MonoDelta::FromSeconds(100));
  }

  stop_threads_.Store(true);

  for (const auto& thr : threads) {
    RETURN_NOT_OK(ThreadJoiner(thr.get()).Join());
  }
  return Status::OK();
}

} // namespace kudu

int main(int argc, char* argv[]) {
  kudu::ParseCommandLineFlags(&argc, &argv, true);
  kudu::InitGoogleLoggingSafe(argv[0]);

  kudu::TpchRealWorld benchmarker;
  kudu::Status s = benchmarker.Init();
  if (!s.ok()) {
    std::cerr << "Couldn't initialize the benchmarking tool, reason: "<< s.ToString() << std::endl;
    return 1;
  }
  s = benchmarker.Run();
  if (!s.ok()) {
    std::cerr << "Couldn't run the benchmarking tool, reason: "<< s.ToString() << std::endl;
    return 1;
  }
  return 0;
}
