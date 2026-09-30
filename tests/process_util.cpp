#include "process_util.hpp"

#include <cstring>

#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#endif

namespace scrtest {
namespace {

std::string g_cli_path;

#ifdef _WIN32

std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  static_cast<void>(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size));
  return wide;
}

// MSVC command line quoting rules.
std::wstring quote_argument(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring result = L"\"";
  for (auto iterator = argument.begin();; ++iterator) {
    std::size_t backslashes = 0;
    while (iterator != argument.end() && *iterator == L'\\') {
      ++iterator;
      ++backslashes;
    }
    if (iterator == argument.end()) {
      result.append(backslashes * 2, L'\\');
      break;
    }
    if (*iterator == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(*iterator);
    } else {
      result.append(backslashes, L'\\');
      result.push_back(*iterator);
    }
  }
  result.push_back(L'"');
  return result;
}

std::vector<wchar_t> build_environment(const std::vector<std::pair<std::string, std::string>>& overrides) {
  std::vector<std::wstring> entries;
  if (LPWCH block = GetEnvironmentStringsW()) {
    for (LPWCH entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
      std::wstring text(entry);
      const std::size_t equals = text.find(L'=');
      if (equals == 0 || equals == std::wstring::npos) {
        continue;
      }
      const std::wstring name = text.substr(0, equals);
      bool overridden = false;
      for (const auto& override_entry : overrides) {
        if (_wcsicmp(name.c_str(), utf8_to_wide(override_entry.first).c_str()) == 0) {
          overridden = true;
          break;
        }
      }
      if (!overridden) {
        entries.push_back(std::move(text));
      }
    }
    FreeEnvironmentStringsW(block);
  }
  for (const auto& override_entry : overrides) {
    entries.push_back(utf8_to_wide(override_entry.first) + L"=" + utf8_to_wide(override_entry.second));
  }
  std::sort(entries.begin(), entries.end(), [](const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) < 0;
  });
  std::vector<wchar_t> block;
  for (const std::wstring& entry : entries) {
    block.insert(block.end(), entry.begin(), entry.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

std::string read_all(HANDLE pipe) {
  std::string text;
  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    text.append(buffer, read);
  }
  return text;
}

#endif  // _WIN32

}  // namespace

void set_cli_path(const std::string& path) {
  g_cli_path = path;
}

const std::string& cli_path() {
  return g_cli_path;
}

ChildProcess::~ChildProcess() {
#ifdef _WIN32
  if (running_) {
    kill();
    wait_for_exit(nullptr);
  }
  close_read_pipe();
  if (thread_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(thread_));
  }
  if (process_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(process_));
  }
#endif
}

#ifdef _WIN32

bool ChildProcess::start(const std::string& executable, const std::vector<std::string>& arguments,
                         const std::vector<std::pair<std::string, std::string>>& environment, std::string* error) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &attributes, 0)) {
    *error = "CreatePipe failed";
    return false;
  }
  if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    *error = "SetHandleInformation failed";
    return false;
  }

  std::wstring command_line = quote_argument(utf8_to_wide(executable));
  for (const std::string& argument : arguments) {
    command_line += L" ";
    command_line += quote_argument(utf8_to_wide(argument));
  }
  std::vector<wchar_t> command_buffer(command_line.begin(), command_line.end());
  command_buffer.push_back(L'\0');

  std::vector<wchar_t> environment_block = build_environment(environment);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe;
  startup.hStdError = write_pipe;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION information{};
  const BOOL created = CreateProcessW(utf8_to_wide(executable).c_str(), command_buffer.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment_block.data(),
                                      nullptr, &startup, &information);
  CloseHandle(write_pipe);
  if (!created) {
    CloseHandle(read_pipe);
    *error = "CreateProcessW failed with Win32 error " + std::to_string(GetLastError());
    return false;
  }

  process_ = information.hProcess;
  thread_ = information.hThread;
  read_pipe_ = read_pipe;
  running_ = true;
  exit_code_ = -1;
  return true;
}

void ChildProcess::close_read_pipe() {
  if (read_pipe_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(read_pipe_));
    read_pipe_ = nullptr;
  }
}

std::string ChildProcess::read_line() {
  if (read_pipe_ == nullptr) {
    return std::string();
  }
  std::string line;
  char character = 0;
  DWORD read = 0;
  while (true) {
    if (!ReadFile(static_cast<HANDLE>(read_pipe_), &character, 1, &read, nullptr) || read == 0) {
      close_read_pipe();
      return line;
    }
    if (character == '\n') {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return line;
    }
    line.push_back(character);
    if (line.size() > (1u << 20)) {
      return line;
    }
  }
}

std::string ChildProcess::read_remaining() {
  if (read_pipe_ == nullptr) {
    return std::string();
  }
  const std::string text = read_all(static_cast<HANDLE>(read_pipe_));
  close_read_pipe();
  return text;
}

bool ChildProcess::wait_for_exit(int* exit_code) {
  if (process_ == nullptr) {
    return false;
  }
  WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  if (!GetExitCodeProcess(static_cast<HANDLE>(process_), &code)) {
    return false;
  }
  exit_code_ = static_cast<int>(code);
  running_ = false;
  if (exit_code != nullptr) {
    *exit_code = exit_code_;
  }
  return true;
}

void ChildProcess::kill() {
  if (process_ == nullptr || !running_) {
    return;
  }
  static_cast<void>(TerminateProcess(static_cast<HANDLE>(process_), 137));
  running_ = false;
}

#else

bool ChildProcess::start(const std::string&, const std::vector<std::string>&,
                         const std::vector<std::pair<std::string, std::string>>&, std::string* error) {
  *error = "process tests are implemented for Windows in this release";
  return false;
}

void ChildProcess::close_read_pipe() {}

std::string ChildProcess::read_line() {
  return std::string();
}

std::string ChildProcess::read_remaining() {
  return std::string();
}

bool ChildProcess::wait_for_exit(int*) {
  return false;
}

void ChildProcess::kill() {}

#endif  // _WIN32

ProcessResult run_process(const std::string& executable, const std::vector<std::string>& arguments,
                          const std::vector<std::pair<std::string, std::string>>& environment) {
  ProcessResult result;
  ChildProcess child;
  std::string error;
  if (!child.start(executable, arguments, environment, &error)) {
    result.start_error = error;
    return result;
  }
  result.started = true;
  result.standard_output = child.read_remaining();
  if (!child.wait_for_exit(&result.exit_code)) {
    result.started = false;
    result.start_error = "the child process could not be waited for";
  }
  return result;
}

}  // namespace scrtest
