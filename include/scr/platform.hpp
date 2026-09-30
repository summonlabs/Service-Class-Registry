#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "scr/result.hpp"

// Operating-system primitives used by the durable store: bounded reads, durable
// atomic writes, directory inspection, real single-writer exclusion, and abrupt
// process termination for the documented crash-consistency fault injection.
//
// Every function reports failure through Status; nothing in this layer throws.
// Paths are always supplied as std::filesystem::path values that were built by
// path_from_utf8, so invalid UTF-8 and embedded NUL bytes are refused at the API
// boundary rather than reaching the operating system.

namespace scr::platform {

// Converts UTF-8 text to a native path. Rejects invalid UTF-8, embedded NUL
// bytes, and empty text.
Result<std::filesystem::path> path_from_utf8(std::string_view text);

// UTF-8 rendering of a native path, for diagnostics.
std::string path_to_utf8(const std::filesystem::path& path);

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes);
Result<std::uint64_t> file_size(const std::filesystem::path& path);
Result<bool> path_exists(const std::filesystem::path& path);
Result<bool> is_directory(const std::filesystem::path& path);
// True for symlinks, junctions, mount points, and any other reparse point. The
// store refuses to operate on a root that is one, so that a link cannot be used
// to redirect durable state.
Result<bool> is_reparse_point(const std::filesystem::path& path);
Result<void> create_directories(const std::filesystem::path& path);
Result<void> remove_file(const std::filesystem::path& path);
Result<std::vector<std::filesystem::path>> list_files(const std::filesystem::path& directory);
// Regular files only (list_files) or subdirectories only (list_directories).
Result<std::vector<std::filesystem::path>> list_directories(const std::filesystem::path& directory);
// Removes every regular file directly inside the directory (not recursively).
Result<std::size_t> remove_files_in(const std::filesystem::path& directory);

// Step one of the durable publication protocol: creates staging_path, writes
// exactly the supplied bytes, flushes them to the device, and closes the file.
// The staging file is never the authoritative copy.
Result<void> write_file_staged(const std::filesystem::path& staging_path, const std::vector<std::uint8_t>& bytes);

// Step two: atomically renames the staged file onto final_path. When
// replace_existing is false an existing final_path is never overwritten.
Result<void> publish_staged(const std::filesystem::path& staging_path, const std::filesystem::path& final_path,
                            bool replace_existing);

std::string process_id_text();

// Terminates the current process without unwinding, flushing, or running
// destructors. Used by the fault-injection facility (environment variable
// SCR_FAULT_INJECT) and by the crash-consistency test suite.
[[noreturn]] void terminate_process_immediately(int exit_code);

enum class LockMode { Shared, Exclusive };

// Real operating-system exclusion:
//   Windows: CreateFileW with a share mode that denies the conflicting access.
//            The handle is closed by the kernel when the process dies, which
//            releases the lock.
//   POSIX:   flock(LOCK_EX | LOCK_NB) or flock(LOCK_SH | LOCK_NB). The kernel
//            releases the lock when the file descriptor is closed on process
//            death.
class LockFile {
 public:
  LockFile() = default;
  ~LockFile();

  LockFile(LockFile&& other) noexcept;
  LockFile& operator=(LockFile&& other) noexcept;
  LockFile(const LockFile&) = delete;
  LockFile& operator=(const LockFile&) = delete;

  static Result<LockFile> acquire(const std::filesystem::path& path, LockMode mode);

  Result<void> release();
  bool held() const noexcept { return held_; }

 private:
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int descriptor_ = -1;
#endif
  bool held_ = false;
};

}  // namespace scr::platform
