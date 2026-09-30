#pragma once

#include <string>
#include <utility>
#include <vector>

namespace scrtest {

// Path of the scr executable under test, supplied by the CTest invocation.
void set_cli_path(const std::string& path);
const std::string& cli_path();

struct ProcessResult {
  bool started = false;
  int exit_code = -1;
  std::string standard_output;
  std::string standard_error;
  std::string start_error;
};

// Runs a child process to completion with its output captured. Standard output
// and standard error share one pipe, so diagnostics appear in
// standard_output and standard_error stays empty; reading the two streams
// separately would risk a deadlock if the child filled one pipe while the
// parent waited on the other. The environment is inherited from the test
// process and then amended with the supplied overrides. No timeout is applied:
// a child that does not finish is a defect to diagnose rather than something to
// hide behind a watchdog.
ProcessResult run_process(const std::string& executable, const std::vector<std::string>& arguments,
                          const std::vector<std::pair<std::string, std::string>>& environment = {});

// A child process that stays alive until it is killed or exits on its own.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  bool start(const std::string& executable, const std::vector<std::string>& arguments,
             const std::vector<std::pair<std::string, std::string>>& environment, std::string* error);

  // Blocks until a complete line is available on the child's standard output.
  // Returns an empty string once the output stream is closed.
  std::string read_line();
  std::string read_remaining();

  bool wait_for_exit(int* exit_code);
  // Abrupt termination with no cleanup, used to prove that the kernel releases
  // the store lock when the holder dies.
  void kill();

  bool running() const { return running_; }

 private:
  void close_read_pipe();

  void* process_ = nullptr;
  void* thread_ = nullptr;
  void* read_pipe_ = nullptr;
  bool running_ = false;
  int exit_code_ = -1;
};

}  // namespace scrtest
