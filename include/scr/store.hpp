#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "scr/platform.hpp"
#include "scr/registry.hpp"

namespace scr {

// ---------------------------------------------------------------------------
// Durable store
//
// Layout of a store root:
//   LOCK              real OS single-writer exclusion (released by the kernel
//                     when the holder dies)
//   manifest          canonical frame, kind StoreManifest: the commit point
//   guard             canonical frame, kind StoreGuard: sequence/epoch fence
//   records/<hex>.scr immutable revision frames, named by their own SHA-256
//   staging/          staging area for atomic publication
//
// Commit protocol: stage, flush, read back and verify, then atomically rename.
// The manifest rename is the single commit point. The guard is advanced only
// after the manifest publication it protects, so a manifest ahead of the guard
// is the expected crash window while a guard ahead of the manifest is a rollback
// and is refused. Recovery selects exactly one authoritative generation and
// fails closed when durable state exists but cannot be verified.
// ---------------------------------------------------------------------------

enum class StoreMode : std::uint8_t { ReadWrite = 1, ReadOnly = 2 };
const char* to_string(StoreMode mode) noexcept;

struct StoreInfo {
  std::filesystem::path root;
  StoreMode mode = StoreMode::ReadOnly;
  AuthorityId authority;
  Epoch epoch;
  Sequence sequence;
  Digest manifest_digest;
  // Digest of the index contents only (classes, history, digests), excluding the
  // commit sequence and write time. An unchanged index digest means a commit
  // would be an idempotent no-op.
  Digest index_digest;
  Timestamp written_at;
  std::size_t class_count = 0;
  std::size_t revision_count = 0;
  std::size_t record_files = 0;
  std::size_t orphan_records = 0;
  std::size_t staging_files_removed = 0;
  bool guard_repaired = false;
  bool lock_held_exclusive = false;
};

struct CommitOutcome {
  Sequence sequence;
  Digest manifest_digest;
  std::size_t records_written = 0;
  bool idempotent = false;
};

struct StoreVerifyReport {
  bool ok = false;
  Sequence sequence;
  Digest manifest_digest;
  std::size_t referenced_records = 0;
  std::size_t verified_records = 0;
  std::size_t orphan_records = 0;
  std::size_t staging_files = 0;
  std::vector<std::string> problems;
};

struct StorePublishOutcome {
  PublishOutcome publish;
  CommitOutcome commit;
};

struct StoreRetireOutcome {
  RetireOutcome retire;
  CommitOutcome commit;
};

class Store {
 public:
  Store() = default;
  ~Store() = default;

  Store(Store&&) noexcept = default;
  Store& operator=(Store&&) noexcept = default;
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Creates a new store. Refuses an already initialized store, a reparse point
  // root, and a non-empty directory that is not a store.
  static Result<Store> create(const std::filesystem::path& root, const AuthorityId& authority, Epoch epoch,
                              const Timestamp& at);

  // Opens an existing store, verifying the manifest, the guard fence, and every
  // referenced record before anything is exposed. Read-only mode takes a shared
  // lock and performs no mutation; read-write mode takes the exclusive lock.
  static Result<Store> open(const std::filesystem::path& root, StoreMode mode);

  Registry& registry() noexcept { return registry_; }
  const Registry& registry() const noexcept { return registry_; }
  const StoreInfo& info() const noexcept { return info_; }

  // Publishes in memory, then commits durably. If the commit fails, the
  // in-memory publication is rolled back so that durable and in-memory state
  // never diverge.
  Result<StorePublishOutcome> publish(const ClassRevision& content, const PublishOptions& options);
  Result<StoreRetireOutcome> retire(const ClassId& class_id, const BoundedText& reason, const Timestamp& at);

  Result<CommitOutcome> commit(const Timestamp& at);

  // Publishes a new control epoch atomically with the state that makes it
  // authoritative. The new epoch fences any writer that still holds the old one.
  Result<CommitOutcome> adopt_epoch(Epoch epoch, const Timestamp& at);

  Result<StoreVerifyReport> verify() const;

  // Removes staging residue and record files that the manifest does not
  // reference. Verified before and after, so compaction can never produce a
  // state that a normal reader would refuse.
  Result<std::size_t> compact();

 private:
  std::filesystem::path root_;
  StoreInfo info_;
  Registry registry_;
  platform::LockFile lock_;
  std::vector<std::uint8_t> manifest_frame_;

  Status load_locked(StoreMode mode);
  Result<CommitOutcome> commit_locked(const Timestamp& at);
  Result<std::size_t> ensure_records(const std::vector<ClassRecord>& records);
  Result<std::size_t> remove_staging_files();
  Result<std::vector<std::filesystem::path>> record_files() const;
};

}  // namespace scr
