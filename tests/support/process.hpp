// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Real OS process control for the multiprocess fencing test.
#ifndef NAF_TEST_PROCESS_HPP
#define NAF_TEST_PROCESS_HPP

#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace naftest {

/// A real child process. start() launches it; kill() terminates it abruptly,
/// which is how a crashed coordinator is simulated.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() { (void)kill(); }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  bool start(const std::string& executable, const std::vector<std::string>& arguments) {
#if defined(_WIN32)
    std::string command_line = "\"" + executable + "\"";
    for (const auto& argument : arguments) command_line += " \"" + argument + "\"";
    std::vector<char> mutable_line(command_line.begin(), command_line.end());
    mutable_line.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &startup, &info) == 0) {
      return false;
    }
    process_ = info.hProcess;
    thread_ = info.hThread;
    return true;
#else
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, executable.c_str(), nullptr, nullptr, argv.data(), environ) != 0) return false;
    pid_ = pid;
    return true;
#endif
  }

  bool running() const {
#if defined(_WIN32)
    if (process_ == nullptr) return false;
    return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
    if (pid_ <= 0) return false;
    int status = 0;
    return waitpid(pid_, &status, WNOHANG) == 0;
#endif
  }

  /// Terminates the process abruptly. Returns true when the process ended.
  bool kill() {
#if defined(_WIN32)
    if (process_ == nullptr) return true;
    const bool killed = TerminateProcess(process_, 137) != 0;
    (void)WaitForSingleObject(process_, 30000);
    CloseHandle(process_);
    if (thread_ != nullptr) CloseHandle(thread_);
    process_ = nullptr;
    thread_ = nullptr;
    return killed;
#else
    if (pid_ <= 0) return true;
    (void)::kill(pid_, SIGKILL);
    int status = 0;
    (void)waitpid(pid_, &status, 0);
    pid_ = -1;
    return true;
#endif
  }

 private:
#if defined(_WIN32)
  void* process_ = nullptr;
  void* thread_ = nullptr;
#else
  int pid_ = -1;
#endif
};

}  // namespace naftest

#endif  // NAF_TEST_PROCESS_HPP
