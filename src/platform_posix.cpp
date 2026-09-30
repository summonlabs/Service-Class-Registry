// POSIX backend for the platform layer: bounded reads, durable staged writes,
// atomic publication, and flock based single-writer exclusion.
//
// This backend is selected automatically on non-Windows hosts. It has NOT been
// validated by the release validation runs recorded in README.md, because the
// release host has no POSIX toolchain; README.md states that limitation
// explicitly. The Windows backend is the validated one.

#include "scr/platform.hpp"

#ifndef _WIN32

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

#include "scr/types.hpp"

namespace scr::platform {
namespace {

Status posix_failure(const char* context, const std::filesystem::path& path, int error) {
  return Status::failure(ErrorCode::IoFailure,
                         std::string(context) + " failed for '" + path.string() + "': " + std::strerror(error), "io");
}

}  // namespace

Result<std::filesystem::path> path_from_utf8(std::string_view text) {
  if (text.empty()) {
    return Result<std::filesystem::path>::failure(Status::failure(ErrorCode::InvalidArgument, "path is empty", "path"));
  }
  if (text.find('\0') != std::string_view::npos) {
    return Result<std::filesystem::path>::failure(
        Status::failure(ErrorCode::InvalidArgument, "path contains an embedded NUL byte", "path"));
  }
  if (text.size() > 4096) {
    return Result<std::filesystem::path>::failure(
        Status::failure(ErrorCode::BoundExceeded, "path exceeds the platform limit", "path"));
  }
  if (!is_valid_utf8(text)) {
    return Result<std::filesystem::path>::failure(
        Status::failure(ErrorCode::InvalidTextEncoding, "path is not well-formed UTF-8", "path"));
  }
  return Result<std::filesystem::path>::success(std::filesystem::path(std::string(text)));
}

std::string path_to_utf8(const std::filesystem::path& path) {
  return path.string();
}

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return Result<std::vector<std::uint8_t>>::failure(posix_failure("open", path, errno));
  }
  struct stat info {};
  if (::fstat(descriptor, &info) != 0) {
    const int error = errno;
    ::close(descriptor);
    return Result<std::vector<std::uint8_t>>::failure(posix_failure("stat", path, error));
  }
  if (!S_ISREG(info.st_mode)) {
    ::close(descriptor);
    return Result<std::vector<std::uint8_t>>::failure(Status::failure(
        ErrorCode::InvalidArgument, "path is not a regular file", path_to_utf8(path)));
  }
  const auto size = static_cast<std::uint64_t>(info.st_size);
  if (size > max_bytes) {
    ::close(descriptor);
    return Result<std::vector<std::uint8_t>>::failure(Status::failure(
        ErrorCode::BoundExceeded,
        "file '" + path_to_utf8(path) + "' is " + std::to_string(size) + " bytes, limit is " +
            std::to_string(max_bytes),
        "io"));
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t read_count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (read_count < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int error = errno;
      ::close(descriptor);
      return Result<std::vector<std::uint8_t>>::failure(posix_failure("read", path, error));
    }
    if (read_count == 0) {
      ::close(descriptor);
      return Result<std::vector<std::uint8_t>>::failure(Status::failure(
          ErrorCode::TruncatedInput, "file ended before its declared size", path_to_utf8(path)));
    }
    offset += static_cast<std::size_t>(read_count);
  }
  ::close(descriptor);
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    return Result<std::uint64_t>::failure(posix_failure("stat", path, errno));
  }
  if (!S_ISREG(info.st_mode)) {
    return Result<std::uint64_t>::failure(
        Status::failure(ErrorCode::InvalidArgument, "path is not a regular file", path_to_utf8(path)));
  }
  return Result<std::uint64_t>::success(static_cast<std::uint64_t>(info.st_size));
}

Result<bool> path_exists(const std::filesystem::path& path) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
      return Result<bool>::success(false);
    }
    return Result<bool>::failure(posix_failure("lstat", path, errno));
  }
  return Result<bool>::success(true);
}

Result<bool> is_directory(const std::filesystem::path& path) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
      return Result<bool>::success(false);
    }
    return Result<bool>::failure(posix_failure("stat", path, errno));
  }
  return Result<bool>::success(S_ISDIR(info.st_mode));
}

Result<bool> is_reparse_point(const std::filesystem::path& path) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
      return Result<bool>::success(false);
    }
    return Result<bool>::failure(posix_failure("lstat", path, errno));
  }
  return Result<bool>::success(S_ISLNK(info.st_mode));
}

Result<void> create_directories(const std::filesystem::path& path) {
  std::error_code error;
  static_cast<void>(std::filesystem::create_directories(path, error));
  if (error) {
    return Result<void>::failure(Status::failure(
        ErrorCode::IoFailure, "cannot create directory '" + path_to_utf8(path) + "': " + error.message(), "io"));
  }
  return ok_result();
}

Result<void> remove_file(const std::filesystem::path& path) {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    return Result<void>::failure(posix_failure("unlink", path, errno));
  }
  return ok_result();
}

Result<std::vector<std::filesystem::path>> list_files(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> files;
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) {
    return Result<std::vector<std::filesystem::path>>::failure(Status::failure(
        ErrorCode::IoFailure, "cannot enumerate '" + path_to_utf8(directory) + "': " + error.message(), "io"));
  }
  for (const std::filesystem::directory_entry& entry : iterator) {
    std::error_code status_error;
    if (entry.is_regular_file(status_error) && !status_error) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return Result<std::vector<std::filesystem::path>>::success(std::move(files));
}

Result<std::vector<std::filesystem::path>> list_directories(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> directories;
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) {
    return Result<std::vector<std::filesystem::path>>::failure(Status::failure(
        ErrorCode::IoFailure, "cannot enumerate '" + path_to_utf8(directory) + "': " + error.message(), "io"));
  }
  for (const std::filesystem::directory_entry& entry : iterator) {
    std::error_code status_error;
    if (entry.is_directory(status_error) && !status_error) {
      directories.push_back(entry.path());
    }
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
  const int descriptor = ::open(staging_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return Result<void>::failure(posix_failure("open", staging_path, errno));
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int error = errno;
      ::close(descriptor);
      static_cast<void>(remove_file(staging_path));
      return Result<void>::failure(posix_failure("write", staging_path, error));
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    const int error = errno;
    ::close(descriptor);
    static_cast<void>(remove_file(staging_path));
    return Result<void>::failure(posix_failure("fsync", staging_path, error));
  }
  if (::close(descriptor) != 0) {
    static_cast<void>(remove_file(staging_path));
    return Result<void>::failure(posix_failure("close", staging_path, errno));
  }
  return ok_result();
}

Result<void> publish_staged(const std::filesystem::path& staging_path, const std::filesystem::path& final_path,
                            bool replace_existing) {
  if (replace_existing) {
    if (::rename(staging_path.c_str(), final_path.c_str()) != 0) {
      const Status failure = posix_failure("rename", final_path, errno);
      static_cast<void>(remove_file(staging_path));
      return Result<void>::failure(failure);
    }
    return ok_result();
  }
  // link() refuses to overwrite, which is exactly the content-addressed publish
  // semantics: an existing record is never replaced.
  if (::link(staging_path.c_str(), final_path.c_str()) != 0) {
    const Status failure = posix_failure("link", final_path, errno);
    static_cast<void>(remove_file(staging_path));
    return Result<void>::failure(failure);
  }
  if (::unlink(staging_path.c_str()) != 0) {
    return Result<void>::failure(posix_failure("unlink", staging_path, errno));
  }
  return ok_result();
}

std::string process_id_text() {
  return std::to_string(static_cast<long>(::getpid()));
}

void terminate_process_immediately(int exit_code) {
  // SIGKILL is an abrupt, non-unwinding death: no destructors, no flush, and no
  // cleanup. The kernel closes the file descriptors, which is what releases the
  // store lock.
  static_cast<void>(::kill(::getpid(), SIGKILL));
  ::_exit(exit_code);
}

LockFile::~LockFile() {
  static_cast<void>(release());
}

LockFile::LockFile(LockFile&& other) noexcept : descriptor_(other.descriptor_), held_(other.held_) {
  other.descriptor_ = -1;
  other.held_ = false;
}

LockFile& LockFile::operator=(LockFile&& other) noexcept {
  if (this != &other) {
    static_cast<void>(release());
    descriptor_ = other.descriptor_;
    held_ = other.held_;
    other.descriptor_ = -1;
    other.held_ = false;
  }
  return *this;
}

Result<LockFile> LockFile::acquire(const std::filesystem::path& path, LockMode mode) {
  const int flags = (mode == LockMode::Exclusive) ? (O_RDWR | O_CREAT | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC);
  const int descriptor = ::open(path.c_str(), flags, 0600);
  if (descriptor < 0) {
    return Result<LockFile>::failure(posix_failure("open", path, errno));
  }
  const int operation = (mode == LockMode::Exclusive) ? (LOCK_EX | LOCK_NB) : (LOCK_SH | LOCK_NB);
  if (::flock(descriptor, operation) != 0) {
    const int error = errno;
    ::close(descriptor);
    if (error == EWOULDBLOCK) {
      return Result<LockFile>::failure(Status::failure(
          ErrorCode::StoreBusy,
          "another process holds the store lock '" + path_to_utf8(path) + "' in a conflicting mode", "store"));
    }
    return Result<LockFile>::failure(posix_failure("flock", path, error));
  }

  LockFile lock;
  lock.descriptor_ = descriptor;
  lock.held_ = true;

  if (mode == LockMode::Exclusive) {
    const std::string owner = process_id_text() + "\n";
    static_cast<void>(::ftruncate(descriptor, 0));
    static_cast<void>(::pwrite(descriptor, owner.data(), owner.size(), 0));
    static_cast<void>(::fsync(descriptor));
  }
  return Result<LockFile>::success(std::move(lock));
}

Result<void> LockFile::release() {
  if (!held_ || descriptor_ < 0) {
    return ok_result();
  }
  const int descriptor = descriptor_;
  descriptor_ = -1;
  held_ = false;
  if (::close(descriptor) != 0) {
    return Result<void>::failure(posix_failure("close", std::filesystem::path("LOCK"), errno));
  }
  return ok_result();
}

}  // namespace scr::platform

#endif  // !_WIN32
