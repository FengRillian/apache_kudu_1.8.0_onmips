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
#ifndef KUDU_UTIL_SUBPROCESS_H
#define KUDU_UTIL_SUBPROCESS_H

#include <signal.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include <gtest/gtest_prod.h>

#include "kudu/gutil/macros.h"
#include "kudu/gutil/port.h"
#include "kudu/util/status.h"

namespace kudu {

// Wrapper around a spawned subprocess.
//
// program will be treated as an absolute path unless it begins with a dot or a
// slash.
//
// This takes care of creating pipes to/from the subprocess and offers
// basic functionality to wait on it or send signals.
// By default, child process only has stdin captured and separate from the parent.
// The stdout/stderr streams are shared with the parent by default.
//
// The process may only be started and waited on/killed once.
//
// Optionally, user may change parent/child stream sharing. Also, a user may disable
// a subprocess stream. A user cannot do both.
//
// Note that, when the Subprocess object is destructed, the child process
// will be forcibly SIGKILLed to avoid orphaning processes.
class Subprocess {
 public:
  // Constructs a new Subprocess that will execute 'argv' on Start().
  //
  // If the process isn't explicitly killed, 'sig_on_destroy' will be delivered
  // to it when the Subprocess goes out of scope.
  explicit Subprocess(std::vector<std::string> argv, int sig_on_destruct = SIGKILL);
  ~Subprocess();

  // Disables subprocess stream output. Is mutually exclusive with stream sharing.
  //
  // Must be called before subprocess starts.
  void DisableStderr();
  void DisableStdout();

  // Configures the subprocess to share the parent's stream. Is mutually
  // exclusive with stream disabling.
  //
  // Must be called before subprocess starts.
  void ShareParentStdin(bool  share = true) { SetFdShared(STDIN_FILENO,  share); }
  void ShareParentStdout(bool share = true) { SetFdShared(STDOUT_FILENO, share); }
  void ShareParentStderr(bool share = true) { SetFdShared(STDERR_FILENO, share); }

  // Add environment variables to be set before executing the subprocess.
  //
  // These environment variables are merged into the existing environment
  // of the parent process. In other words, there is no need to prime this
  // map with the current environment; instead, just specify any variables
  // that should be overridden.
  //
  // Repeated calls to this function replace earlier calls.
  void SetEnvVars(std::map<std::string, std::string> env);

  // Set the initial current working directory of the subprocess.
  //
  // Must be set before starting the subprocess.
  void SetCurrentDir(std::string cwd);

  // Start the subprocess. Can only be called once.
  //
  // This returns a bad Status if the fork() fails. However,
  // note that if the executable path was incorrect such that
  // exec() fails, this will still return Status::OK. You must
  // use Wait() to check for failure.
  Status Start() WARN_UNUSED_RESULT;

  // Wait for the subprocess to exit. The return value is the same as
  // that of the waitpid() syscall. Only call after starting.
  //
  // NOTE: unlike the standard wait(2) call, this may be called multiple
  // times. If the process has exited, it will repeatedly return the same
  // exit code.
  Status Wait(int* wait_status = nullptr) WARN_UNUSED_RESULT;

  // Like the above, but does not block. This returns Status::TimedOut
  // immediately if the child has not exited. Otherwise returns Status::OK
  // and sets *ret. Only call after starting.
  //
  // NOTE: unlike the standard wait(2) call, this may be called multiple
  // times. If the process has exited, it will repeatedly return the same
  // exit code.
  Status WaitNoBlock(int* wait_status = nullptr) WARN_UNUSED_RESULT;

  // Send a signal to the subprocess.
  // Note that this does not reap the process -- you must still Wait()
  // in order to reap it. Only call after starting.
  Status Kill(int signal) WARN_UNUSED_RESULT;

  // Sends a signal to the subprocess and waits for it to exit.
  //
  // If the signal is not SIGKILL and the process doesn't appear to be exiting,
  // retries with SIGKILL.
  Status KillAndWait(int signal);

  // Retrieve exit status of the process awaited by Wait() and/or WaitNoBlock()
  // methods. Must be called only after calling Wait()/WaitNoBlock().
  Status GetExitStatus(int* exit_status, std::string* info_str = nullptr) const
      WARN_UNUSED_RESULT;

  // Helper method that creates a Subprocess, issues a Start() then a Wait().
  // Expects a blank-separated list of arguments, with the first being the
  // full path to the executable.
  // The returned Status will only be OK if all steps were successful and
  // the return code was 0.
  static Status Call(const std::string& arg_str) WARN_UNUSED_RESULT;

  // Same as above, but accepts a vector that includes the path to the
  // executable as argv[0] and the arguments to the program in argv[1..n].
  //
  // Writes the value of 'stdin_in' to the subprocess' stdin. The length of
  // 'stdin_in' should be limited to 64kib.
  //
  // Also collects the output from the child process stdout and stderr into
  // 'stdout_out' and 'stderr_out' respectively.
  static Status Call(const std::vector<std::string>& argv,
                     const std::string& stdin_in = "",
                     std::string* stdout_out = nullptr,
                     std::string* stderr_out = nullptr) WARN_UNUSED_RESULT;

  // Return the pipe fd to the child's standard stream.
  // Stream should not be disabled or shared.
  int to_child_stdin_fd()    const { return CheckAndOffer(STDIN_FILENO); }
  int from_child_stdout_fd() const { return CheckAndOffer(STDOUT_FILENO); }
  int from_child_stderr_fd() const { return CheckAndOffer(STDERR_FILENO); }

  // Release control of the file descriptor for the child's stream, only if piped.
  // Writes to this FD show up on stdin in the subprocess
  int ReleaseChildStdinFd()  { return ReleaseChildFd(STDIN_FILENO ); }
  // Reads from this FD come from stdout of the subprocess
  int ReleaseChildStdoutFd() { return ReleaseChildFd(STDOUT_FILENO); }
  // Reads from this FD come from stderr of the subprocess
  int ReleaseChildStderrFd() { return ReleaseChildFd(STDERR_FILENO); }

  pid_t pid() const;
  const std::string& argv0() const { return argv_[0]; }

 private:
  FRIEND_TEST(SubprocessTest, TestGetProcfsState);

  enum State {
    kNotStarted,
    kRunning,
    kExited
  };
  enum StreamMode {SHARED, DISABLED, PIPED};
  enum WaitMode {BLOCKING, NON_BLOCKING};

  // Process state according to /proc/<pid>/stat.
  enum class ProcfsState {
    // "T  Stopped (on a signal) or (before Linux 2.6.33) trace stopped"
    PAUSED,

    // Every other process state.
    RUNNING,
  };

  // Extracts the process state for /proc/<pid>/stat.
  //
  // Returns an error if /proc/</pid>/stat doesn't exist or if parsing failed.
  static Status GetProcfsState(int pid, ProcfsState* state);

  Status DoWait(int* wait_status, WaitMode mode) WARN_UNUSED_RESULT;
  void SetFdShared(int stdfd, bool share);
  int CheckAndOffer(int stdfd) const;
  int ReleaseChildFd(int stdfd);

  std::string program_;
  std::vector<std::string> argv_;
  std::map<std::string, std::string> env_;
  State state_;
  int child_pid_;
  enum StreamMode fd_state_[3];
  int child_fds_[3];
  std::string cwd_;

  // The cached wait status if Wait()/WaitNoBlock() has been called.
  // Only valid if state_ == kExited.
  int wait_status_;

  // Custom signal to deliver when the subprocess goes out of scope, provided
  // the process hasn't already been killed.
  int sig_on_destruct_;

  DISALLOW_COPY_AND_ASSIGN(Subprocess);
};

} // namespace kudu
#endif /* KUDU_UTIL_SUBPROCESS_H */
