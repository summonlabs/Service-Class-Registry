#include "scr/platform.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdlib>

namespace scr::platform {
namespace {

std::string format_system_message(unsigned long code) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::string text;
  if (length != 0 && buffer != nullptr) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (size > 0) {
      text.resize(static_cast<std::size_t>(size));
      static_cast<void>(WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), text.data(), size, nullptr,
                                            nullptr));
    }
    LocalFree(buffer);
  }
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}

Status win32_failure(const char* context, const std::filesystem::path& path, unsigned long code) {
  std::string detail = std::string(context) + " failed for '";
  detail += path_to_utf8(path);
  detail += "' with Win32 error ";
  detail += std::to_string(code);
  const std::string message = format_system_message(code);
  if (!message.empty()) {
    detail += " (";
    detail += message;
    detail += ")";
  }
  return Status::failure(ErrorCode::IoFailure, std::move(detail), "io");
}

// Native path text with the extended-length prefix applied when needed, so that
// long paths do not silently depend on the machine-wide long path setting.
std::wstring native_path(const std::filesystem::path& path) {
  std::wstring text = path.wstring();
  std::replace(text.begin(), text.end(), L'/', L'\\');
  if (text.rfind(L"\\\\?\\", 0) == 0) {
    return text;
  }
  const bool absolute_drive = text.size() >= 2 && text[1] == L':' && ((text[0] >= L'A' && text[0] <= L'Z') ||
                                                                     (text[0] >= L'a' && text[0] <= L'z'));
  const bool absolute_unc = text.rfind(L"\\\\", 0) == 0;
  if (text.size() >= 240) {
    if (absolute_unc) {
      return L"\\\\?\\UNC\\" + text.substr(2);
    }
    if (absolute_drive) {
      return L"\\\\?\\" + text;
    }
  }
  if (absolute_drive && text.size() >= 3 && text[2] == L'\\') {
    // Keep the drive-relative form untouched; only fully qualified paths are
    // eligible for the extended prefix.
    return text;
  }
  return text;
}

Result<void> write_all(HANDLE handle, const std::vector<std::uint8_t>& bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    const DWORD chunk = static_cast<DWORD>((remaining > 0x10000000u) ? 0x10000000u : remaining);
    DWORD written = 0;
    if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr)) {
      const DWORD code = GetLastError();
      return Result<void>::failure(Status::failure(
          ErrorCode::IoFailure, "WriteFile failed with Win32 error " + std::to_string(code), "io"));
    }
    if (written == 0) {
      return Result<void>::failure(
          Status::failure(ErrorCode::IoFailure, "WriteFile reported no progress", "io"));
    }
    offset += written;
  }
  return ok_result();
}

}  // namespace

Result<std::filesystem::path> path_from_utf8(std::string_view text) {
  if (text.empty()) {
    return Result<std::filesystem::path>::failure(
        Status::failure(ErrorCode::InvalidArgument, "path is empty", "path"));
  }
  if (text.find('\0') != std::string_view::npos) {
    return Result<std::filesystem::path>::failure(
        Status::failure(ErrorCode::InvalidArgument, "path contains an embedded NUL byte", "path"));
  }
  if (text.size() > 32767) {
    return Result<std::filesystem::path>::failure(
        Status::failure(ErrorCode::BoundExceeded, "path exceeds the platform limit", "path"));
  }
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0);
  if (size <= 0) {
    return Result<std::filesystem::path>::failure(
        Status::failure(ErrorCode::InvalidTextEncoding, "path is not well-formed UTF-8", "path"));
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  static_cast<void>(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                        wide.data(), size));
  return Result<std::filesystem::path>::success(std::filesystem::path(std::move(wide)));
}

std::string path_to_utf8(const std::filesystem::path& path) {
  const std::wstring wide = path.wstring();
  if (wide.empty()) {
    return std::string();
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                       nullptr);
  if (size <= 0) {
    return "<unprintable path>";
  }
  std::string text(static_cast<std::size_t>(size), '\0');
  static_cast<void>(WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size,
                                        nullptr, nullptr));
  return text;
}

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes) {
  const std::wstring native = native_path(path);
  HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    return Result<std::vector<std::uint8_t>>::failure(win32_failure("open", path, code));
  }

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(handle, &size)) {
    const Status failure = win32_failure("size query", path, GetLastError());
    CloseHandle(handle);
    return Result<std::vector<std::uint8_t>>::failure(failure);
  }
  if (size.QuadPart < 0) {
    CloseHandle(handle);
    return Result<std::vector<std::uint8_t>>::failure(
        Status::failure(ErrorCode::CorruptStore, "file reports a negative size", path_to_utf8(path)));
  }
  const auto declared = static_cast<std::uint64_t>(size.QuadPart);
  if (declared > max_bytes) {
    CloseHandle(handle);
    return Result<std::vector<std::uint8_t>>::failure(Status::failure(
        ErrorCode::BoundExceeded,
        "file '" + path_to_utf8(path) + "' is " + std::to_string(declared) + " bytes, limit is " +
            std::to_string(max_bytes),
        "io"));
  }

  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(declared));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    const DWORD chunk = static_cast<DWORD>((remaining > 0x10000000u) ? 0x10000000u : remaining);
    DWORD read = 0;
    if (!ReadFile(handle, bytes.data() + offset, chunk, &read, nullptr)) {
      const Status failure = win32_failure("read", path, GetLastError());
      CloseHandle(handle);
      return Result<std::vector<std::uint8_t>>::failure(failure);
    }
    if (read == 0) {
      CloseHandle(handle);
      return Result<std::vector<std::uint8_t>>::failure(Status::failure(
          ErrorCode::TruncatedInput, "file ended before its declared size", path_to_utf8(path)));
    }
    offset += read;
  }
  CloseHandle(handle);
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(native_path(path).c_str(), GetFileExInfoStandard, &data)) {
    const DWORD code = GetLastError();
    return Result<std::uint64_t>::failure(win32_failure("attribute query", path, code));
  }
  if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return Result<std::uint64_t>::failure(
        Status::failure(ErrorCode::InvalidArgument, "path is a directory, not a file", path_to_utf8(path)));
  }
  const ULARGE_INTEGER size{data.nFileSizeLow, data.nFileSizeHigh};
  return Result<std::uint64_t>::success(static_cast<std::uint64_t>(size.QuadPart));
}

Result<bool> path_exists(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(native_path(path).c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_INVALID_NAME) {
      return Result<bool>::success(false);
    }
    return Result<bool>::failure(win32_failure("attribute query", path, code));
  }
  return Result<bool>::success(true);
}

Result<bool> is_directory(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(native_path(path).c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return Result<bool>::success(false);
    }
    return Result<bool>::failure(win32_failure("attribute query", path, code));
  }
  return Result<bool>::success((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

Result<bool> is_reparse_point(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(native_path(path).c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return Result<bool>::success(false);
    }
    return Result<bool>::failure(win32_failure("attribute query", path, code));
  }
  return Result<bool>::success((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
}

Result<void> create_directories(const std::filesystem::path& path) {
  // Components are created one at a time with the extended-length prefix
  // applied where needed, so that deep paths do not depend on the machine-wide
  // long path setting. Existence is probed with GetFileAttributesW rather than
  // std::filesystem, which does not accept paths beyond MAX_PATH unless the
  // process opted in through its manifest.
  const auto existing = scr::platform::is_directory(path);
  if (!existing.ok()) {
    return Result<void>::failure(existing.status());
  }
  if (existing.value()) {
    return ok_result();
  }
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty() && parent != path) {
    const Result<void> created_parent = scr::platform::create_directories(parent);
    if (!created_parent.ok()) {
      return created_parent;
    }
  }
  if (!CreateDirectoryW(native_path(path).c_str(), nullptr)) {
    const DWORD code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS) {
      const auto concurrent = scr::platform::is_directory(path);
      if (concurrent.ok() && concurrent.value()) {
        return ok_result();
      }
    }
    return Result<void>::failure(win32_failure("create directory", path, code));
  }
  return ok_result();
}

Result<void> remove_file(const std::filesystem::path& path) {
  if (!DeleteFileW(native_path(path).c_str())) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return ok_result();
    }
    return Result<void>::failure(win32_failure("delete", path, code));
  }
  return ok_result();
}

Result<std::vector<std::filesystem::path>> list_files(const std::filesystem::path& directory) {
  // FindFirstFileW with the extended-length prefix, so that enumeration works
  // for deep paths and reports a missing directory as a failure rather than as
  // an empty result.
  std::vector<std::filesystem::path> files;
  const std::wstring pattern = native_path(directory / L"*");
  WIN32_FIND_DATAW data{};
  HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    return Result<std::vector<std::filesystem::path>>::failure(win32_failure("enumerate", directory, code));
  }
  do {
    const std::wstring name = data.cFileName;
    if (name == L"." || name == L"..") {
      continue;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    files.push_back(directory / name);
  } while (FindNextFileW(handle, &data) != 0);
  const DWORD last = GetLastError();
  FindClose(handle);
  if (last != ERROR_NO_MORE_FILES) {
    return Result<std::vector<std::filesystem::path>>::failure(win32_failure("enumerate", directory, last));
  }
  std::sort(files.begin(), files.end());
  return Result<std::vector<std::filesystem::path>>::success(std::move(files));
}

Result<std::vector<std::filesystem::path>> list_directories(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> directories;
  const std::wstring pattern = native_path(directory / L"*");
  WIN32_FIND_DATAW data{};
  HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return Result<std::vector<std::filesystem::path>>::success(std::move(directories));
    }
    return Result<std::vector<std::filesystem::path>>::failure(win32_failure("enumerate", directory, code));
  }
  do {
    const std::wstring name = data.cFileName;
    if (name == L"." || name == L"..") {
      continue;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      continue;
    }
    directories.push_back(directory / name);
  } while (FindNextFileW(handle, &data) != 0);
  const DWORD last = GetLastError();
  FindClose(handle);
  if (last != ERROR_NO_MORE_FILES) {
    return Result<std::vector<std::filesystem::path>>::failure(win32_failure("enumerate", directory, last));
  }
  std::sort(directories.begin(), directories.end());
  return Result<std::vector<std::filesystem::path>>::success(std::move(directories));
}

Result<std::size_t> remove_files_in(const std::filesystem::path& directory) {
  const auto files = list_files(directory);
  if (!files.ok()) {
    return Result<std::size_t>::failure(files.status());
  }
  std::size_t removed = 0;
  for (const std::filesystem::path& file : files.value()) {
    const Result<void> result = remove_file(file);
    if (!result.ok()) {
      return Result<std::size_t>::failure(result.status());
    }
    ++removed;
  }
  return Result<std::size_t>::success(removed);
}

Result<void> write_file_staged(const std::filesystem::path& staging_path, const std::vector<std::uint8_t>& bytes) {
  const std::wstring native = native_path(staging_path);
  HANDLE handle = CreateFileW(native.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    return Result<void>::failure(win32_failure("create", staging_path, code));
  }
  const Result<void> written = write_all(handle, bytes);
  if (!written.ok()) {
    CloseHandle(handle);
    static_cast<void>(remove_file(staging_path));
    return Result<void>::failure(written.status());
  }
  if (!FlushFileBuffers(handle)) {
    const Status failure = win32_failure("flush", staging_path, GetLastError());
    CloseHandle(handle);
    static_cast<void>(remove_file(staging_path));
    return Result<void>::failure(failure);
  }
  CloseHandle(handle);
  return ok_result();
}

Result<void> publish_staged(const std::filesystem::path& staging_path, const std::filesystem::path& final_path,
                            bool replace_existing) {
  DWORD flags = MOVEFILE_WRITE_THROUGH;
  if (replace_existing) {
    flags |= MOVEFILE_REPLACE_EXISTING;
  }
  if (!MoveFileExW(native_path(staging_path).c_str(), native_path(final_path).c_str(), flags)) {
    const Status failure = win32_failure("rename", final_path, GetLastError());
    static_cast<void>(remove_file(staging_path));
    return Result<void>::failure(failure);
  }
  return ok_result();
}

std::string process_id_text() {
  return std::to_string(static_cast<unsigned long>(GetCurrentProcessId()));
}

void terminate_process_immediately(int exit_code) {
  static_cast<void>(TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exit_code)));
  // TerminateProcess does not return for the calling process; if it somehow
  // does, fall back to the C runtime's immediate exit.
  std::_Exit(exit_code);
}

LockFile::~LockFile() {
  static_cast<void>(release());
}

LockFile::LockFile(LockFile&& other) noexcept : handle_(other.handle_), held_(other.held_) {
  other.handle_ = nullptr;
  other.held_ = false;
}

LockFile& LockFile::operator=(LockFile&& other) noexcept {
  if (this != &other) {
    static_cast<void>(release());
    handle_ = other.handle_;
    held_ = other.held_;
    other.handle_ = nullptr;
    other.held_ = false;
  }
  return *this;
}

Result<LockFile> LockFile::acquire(const std::filesystem::path& path, LockMode mode) {
  const std::wstring native = native_path(path);
  const DWORD access = (mode == LockMode::Exclusive) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
  // Exclusive: deny every other opener. Shared: allow other readers, deny
  // writers. Both constraints are released by the kernel when the handle is
  // closed, including when the process dies.
  const DWORD share = (mode == LockMode::Exclusive) ? 0u : FILE_SHARE_READ;
  HANDLE handle = CreateFileW(native.c_str(), access, share, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
      return Result<LockFile>::failure(Status::failure(
          ErrorCode::StoreBusy,
          "another process holds the store lock '" + path_to_utf8(path) + "' in a conflicting mode", "store"));
    }
    return Result<LockFile>::failure(win32_failure("lock", path, code));
  }

  LockFile lock;
  lock.handle_ = handle;
  lock.held_ = true;

  if (mode == LockMode::Exclusive) {
    // Advisory owner record; exclusivity itself is enforced by the handle. A
    // failure to write it is not fatal, so the result is deliberately ignored.
    const std::string owner = process_id_text() + "\n";
    DWORD written = 0;
    static_cast<void>(SetFilePointer(handle, 0, nullptr, FILE_BEGIN));
    static_cast<void>(WriteFile(handle, owner.data(), static_cast<DWORD>(owner.size()), &written, nullptr));
    static_cast<void>(FlushFileBuffers(handle));
  }
  return Result<LockFile>::success(std::move(lock));
}

Result<void> LockFile::release() {
  if (!held_ || handle_ == nullptr) {
    return ok_result();
  }
  const HANDLE handle = static_cast<HANDLE>(handle_);
  handle_ = nullptr;
  held_ = false;
  if (!CloseHandle(handle)) {
    return Result<void>::failure(
        Status::failure(ErrorCode::IoFailure, "CloseHandle failed with Win32 error " + std::to_string(GetLastError()),
                        "io"));
  }
  return ok_result();
}

}  // namespace scr::platform
