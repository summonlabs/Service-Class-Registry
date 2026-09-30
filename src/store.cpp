#include "scr/store.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string_view>

#include "scr/bytes.hpp"
#include "scr/canonical.hpp"

namespace scr {
namespace {

constexpr const char* kLockFileName = "LOCK";
constexpr const char* kGuardFileName = "guard";
constexpr const char* kManifestFileName = "manifest";
constexpr const char* kRecordsDirectoryName = "records";
constexpr const char* kStagingDirectoryName = "staging";
constexpr const char* kRecordSuffix = ".scr";
constexpr const char* kStagingSuffix = ".tmp";
constexpr std::uint16_t kManifestPayloadVersion = 1;
constexpr std::uint64_t kMaxRecordFileBytes = kFrameOverheadBytes + kMaxRevisionPayloadBytes + 4096u;
constexpr std::uint64_t kMaxManifestFileBytes = kFrameOverheadBytes + kMaxManifestPayloadBytes + 4096u;
constexpr std::uint64_t kMaxGuardFileBytes = kFrameOverheadBytes + kMaxGuardPayloadBytes;
constexpr std::size_t kMinManifestEntryBytes = 32;

// Crash-consistency fault injection. When SCR_FAULT_INJECT names a commit stage
// the process terminates immediately at that stage without unwinding, which is
// what the crash-recovery tests use to prove that every stage is either fully
// published or invisible to the next open.
void fault_point(const char* point) {
  // std::getenv is used read-only here. MSVC flags it as deprecated for the
  // unsafe mutable-buffer overload; that concern does not apply to reading an
  // environment variable that this process never modifies.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  const char* value = std::getenv("SCR_FAULT_INJECT");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  if (value == nullptr) {
    return;
  }
  if (std::string_view(value) == std::string_view(point)) {
    platform::terminate_process_immediately(137);
  }
}

std::string utf8(const std::filesystem::path& path) {
  return platform::path_to_utf8(path);
}

std::string hex_of(const Digest& digest) {
  return digest.hex();
}

struct ManifestData {
  AuthorityId authority;
  Epoch epoch;
  Sequence sequence;
  Timestamp written_at;
  std::vector<ClassRecord> records;
};

Result<std::vector<std::uint8_t>> encode_manifest_payload(const ManifestData& data) {
  ByteWriter writer;
  writer.put_u16(kManifestPayloadVersion);
  writer.put_u8(0);
  writer.put_string(data.authority.str());
  writer.put_u64(data.epoch.value());
  writer.put_u64(data.sequence.value());
  writer.put_u8(data.written_at.has_value() ? 1 : 0);
  if (data.written_at.has_value()) {
    writer.put_i64(data.written_at.unix_seconds());
  }
  writer.put_u32(static_cast<std::uint32_t>(data.records.size()));
  for (const ClassRecord& record : data.records) {
    writer.put_string(record.class_id.str());
    writer.put_u8(static_cast<std::uint8_t>(record.state));
    writer.put_u64(record.generation.value());
    writer.put_u32(record.tip.value());
    writer.put_bytes(record.tip_digest.bytes().data(), Digest::kBytes);
    writer.put_u8(record.published_at.has_value() ? 1 : 0);
    if (record.published_at.has_value()) {
      writer.put_i64(record.published_at.unix_seconds());
    }
    writer.put_u8(record.retired_at.has_value() ? 1 : 0);
    if (record.retired_at.has_value()) {
      writer.put_i64(record.retired_at.unix_seconds());
    }
    writer.put_string(record.retire_reason.value());
    writer.put_u32(static_cast<std::uint32_t>(record.history.size()));
    for (const ClassHistoryEntry& entry : record.history) {
      writer.put_u64(entry.reference.generation.value());
      writer.put_u32(entry.reference.revision.value());
      writer.put_bytes(entry.reference.digest.bytes().data(), Digest::kBytes);
      writer.put_u8(entry.published_at.has_value() ? 1 : 0);
      if (entry.published_at.has_value()) {
        writer.put_i64(entry.published_at.unix_seconds());
      }
      writer.put_u8(entry.generation_start ? 1 : 0);
    }
  }
  return Result<std::vector<std::uint8_t>>::success(writer.take());
}

Result<Timestamp> take_timestamp(ByteReader& reader, std::string_view subject) {
  const auto flag = reader.take_u8();
  if (!flag.ok()) {
    return Result<Timestamp>::failure(flag.status());
  }
  if (flag.value() == 0) {
    return Result<Timestamp>::success(Timestamp::unset());
  }
  if (flag.value() != 1) {
    return Result<Timestamp>::failure(Status::failure(
        ErrorCode::CorruptStore, "timestamp discriminator for " + std::string(subject) + " is not defined", "store"));
  }
  const auto raw = reader.take_i64();
  if (!raw.ok()) {
    return Result<Timestamp>::failure(raw.status());
  }
  auto stamp = Timestamp::from_unix_seconds(raw.value());
  if (!stamp.ok()) {
    return Result<Timestamp>::failure(Status::failure(
        ErrorCode::CorruptStore, "timestamp for " + std::string(subject) + " is out of range", "store"));
  }
  return stamp;
}

Result<Digest> take_digest_value(ByteReader& reader, std::string_view subject) {
  auto bytes = reader.take_bytes(Digest::kBytes);
  if (!bytes.ok()) {
    return Result<Digest>::failure(bytes.status());
  }
  std::array<std::uint8_t, Digest::kBytes> raw{};
  std::copy(bytes.value().begin(), bytes.value().end(), raw.begin());
  const Digest digest = Digest::from_bytes(raw);
  if (digest.is_null()) {
    return Result<Digest>::failure(Status::failure(
        ErrorCode::CorruptStore, "null digest for " + std::string(subject), "store"));
  }
  return Result<Digest>::success(digest);
}

Result<ManifestData> decode_manifest_payload(const std::vector<std::uint8_t>& payload) {
  ByteReader reader(payload.data(), payload.size());
  const auto version = reader.take_u16();
  if (!version.ok()) {
    return Result<ManifestData>::failure(version.status());
  }
  if (version.value() != kManifestPayloadVersion) {
    return Result<ManifestData>::failure(Status::failure(
        ErrorCode::UnsupportedVersion,
        "manifest payload version " + std::to_string(version.value()) + " is not supported", "store"));
  }
  const auto reserved = reader.take_u8();
  if (!reserved.ok()) {
    return Result<ManifestData>::failure(reserved.status());
  }
  if (reserved.value() != 0) {
    return Result<ManifestData>::failure(
        Status::failure(ErrorCode::CorruptStore, "reserved manifest byte is not zero", "store"));
  }

  ManifestData data;
  {
    const auto authority = reader.take_string(kAuthorityIdMaxBytes);
    if (!authority.ok()) {
      return Result<ManifestData>::failure(authority.status());
    }
    const auto parsed = AuthorityId::parse(authority.value());
    if (!parsed.ok()) {
      return Result<ManifestData>::failure(parsed.status());
    }
    data.authority = parsed.value();
  }
  {
    const auto epoch = reader.take_u64();
    if (!epoch.ok()) {
      return Result<ManifestData>::failure(epoch.status());
    }
    data.epoch = Epoch::from(epoch.value());
    if (data.epoch.is_zero()) {
      return Result<ManifestData>::failure(
          Status::failure(ErrorCode::CorruptStore, "manifest control epoch is zero", "store"));
    }
  }
  {
    const auto sequence = reader.take_u64();
    if (!sequence.ok()) {
      return Result<ManifestData>::failure(sequence.status());
    }
    data.sequence = Sequence::from(sequence.value());
    if (data.sequence.value() == 0) {
      return Result<ManifestData>::failure(
          Status::failure(ErrorCode::CorruptStore, "manifest sequence is zero", "store"));
    }
  }
  {
    const auto written = take_timestamp(reader, "manifest write time");
    if (!written.ok()) {
      return Result<ManifestData>::failure(written.status());
    }
    data.written_at = written.value();
  }

  const auto class_count = reader.take_u32();
  if (!class_count.ok()) {
    return Result<ManifestData>::failure(class_count.status());
  }
  if (class_count.value() > payload.size() / kMinManifestEntryBytes) {
    return Result<ManifestData>::failure(Status::failure(
        ErrorCode::BoundExceeded,
        "declared class count " + std::to_string(class_count.value()) + " exceeds what the payload can hold", "store"));
  }
  data.records.reserve(class_count.value());
  for (std::uint32_t index = 0; index < class_count.value(); ++index) {
    ClassRecord record;
    {
      const auto text = reader.take_string(kClassIdMaxBytes);
      if (!text.ok()) {
        return Result<ManifestData>::failure(text.status());
      }
      const auto parsed = ClassId::parse(text.value());
      if (!parsed.ok()) {
        return Result<ManifestData>::failure(parsed.status());
      }
      record.class_id = parsed.value();
    }
    {
      const auto state = reader.take_u8();
      if (!state.ok()) {
        return Result<ManifestData>::failure(state.status());
      }
      if (state.value() != static_cast<std::uint8_t>(ClassState::Active) &&
          state.value() != static_cast<std::uint8_t>(ClassState::Retired)) {
        return Result<ManifestData>::failure(
            Status::failure(ErrorCode::CorruptStore, "class state is not a defined lifecycle state", "store"));
      }
      record.state = static_cast<ClassState>(state.value());
    }
    {
      const auto generation = reader.take_u64();
      if (!generation.ok()) {
        return Result<ManifestData>::failure(generation.status());
      }
      if (generation.value() == 0) {
        return Result<ManifestData>::failure(
            Status::failure(ErrorCode::CorruptStore, "class generation is zero", "store"));
      }
      record.generation = Generation::from(generation.value());
    }
    {
      const auto tip = reader.take_u32();
      if (!tip.ok()) {
        return Result<ManifestData>::failure(tip.status());
      }
      if (tip.value() == 0) {
        return Result<ManifestData>::failure(Status::failure(ErrorCode::CorruptStore, "class tip is zero", "store"));
      }
      record.tip = Revision::from(tip.value());
    }
    {
      const auto digest = take_digest_value(reader, "class tip");
      if (!digest.ok()) {
        return Result<ManifestData>::failure(digest.status());
      }
      record.tip_digest = digest.value();
    }
    {
      const auto published = take_timestamp(reader, "class publication time");
      if (!published.ok()) {
        return Result<ManifestData>::failure(published.status());
      }
      record.published_at = published.value();
    }
    {
      const auto retired = take_timestamp(reader, "class retirement time");
      if (!retired.ok()) {
        return Result<ManifestData>::failure(retired.status());
      }
      record.retired_at = retired.value();
    }
    {
      const auto reason = reader.take_text(kRetireReasonMaxBytes);
      if (!reason.ok()) {
        return Result<ManifestData>::failure(reason.status());
      }
      if (!reason.value().empty()) {
        const auto created = BoundedText::create(reason.value(), kRetireReasonMaxBytes);
        if (!created.ok()) {
          return Result<ManifestData>::failure(created.status());
        }
        record.retire_reason = created.value();
      }
    }
    const auto history_count = reader.take_u32();
    if (!history_count.ok()) {
      return Result<ManifestData>::failure(history_count.status());
    }
    if (history_count.value() > payload.size() / kMinManifestEntryBytes) {
      return Result<ManifestData>::failure(Status::failure(
          ErrorCode::BoundExceeded, "declared history count exceeds what the payload can hold", "store"));
    }
    record.history.reserve(history_count.value());
    for (std::uint32_t position = 0; position < history_count.value(); ++position) {
      ClassHistoryEntry entry;
      {
        const auto generation = reader.take_u64();
        if (!generation.ok()) {
          return Result<ManifestData>::failure(generation.status());
        }
        entry.reference.generation = Generation::from(generation.value());
      }
      {
        const auto revision = reader.take_u32();
        if (!revision.ok()) {
          return Result<ManifestData>::failure(revision.status());
        }
        entry.reference.revision = Revision::from(revision.value());
      }
      {
        const auto digest = take_digest_value(reader, "history entry");
        if (!digest.ok()) {
          return Result<ManifestData>::failure(digest.status());
        }
        entry.reference.digest = digest.value();
      }
      {
        const auto published = take_timestamp(reader, "history publication time");
        if (!published.ok()) {
          return Result<ManifestData>::failure(published.status());
        }
        entry.published_at = published.value();
      }
      {
        const auto marker = reader.take_u8();
        if (!marker.ok()) {
          return Result<ManifestData>::failure(marker.status());
        }
        if (marker.value() > 1) {
          return Result<ManifestData>::failure(Status::failure(
              ErrorCode::CorruptStore, "history generation marker is not defined", "store"));
        }
        entry.generation_start = marker.value() == 1;
      }
      record.history.push_back(std::move(entry));
    }
    if (record.history.empty()) {
      return Result<ManifestData>::failure(
          Status::failure(ErrorCode::CorruptStore, "class record has an empty history", "store"));
    }
    data.records.push_back(std::move(record));
  }

  if (reader.remaining() != 0) {
    return Result<ManifestData>::failure(Status::failure(
        ErrorCode::CorruptStore,
        "manifest payload has " + std::to_string(reader.remaining()) + " trailing bytes", "store"));
  }
  return Result<ManifestData>::success(std::move(data));
}

// Digest of the index contents only. The commit sequence and the write time are
// deliberately excluded so that an unchanged index is recognisable as an
// idempotent commit.
Result<Digest> index_digest_of(const ManifestData& data) {
  ManifestData bare = data;
  bare.sequence = Sequence::from(0);
  bare.written_at = Timestamp::unset();
  const auto payload = encode_manifest_payload(bare);
  if (!payload.ok()) {
    return Result<Digest>::failure(payload.status());
  }
  return Result<Digest>::success(Digest::of(payload.value().data(), payload.value().size()));
}

bool parse_record_file_stem(const std::filesystem::path& path, Digest* out) {
  const std::string stem = path.filename().string();
  if (stem.size() != Digest::kHexChars + 4) {
    return false;
  }
  if (stem.compare(Digest::kHexChars, 4, kRecordSuffix) != 0) {
    return false;
  }
  const auto parsed = Digest::parse(std::string_view(stem).substr(0, Digest::kHexChars));
  if (!parsed.ok()) {
    return false;
  }
  if (out != nullptr) {
    *out = parsed.value();
  }
  return true;
}

}  // namespace

const char* to_string(StoreMode mode) noexcept {
  switch (mode) {
    case StoreMode::ReadWrite:
      return "read-write";
    case StoreMode::ReadOnly:
      return "read-only";
  }
  return "unknown";
}

Result<Store> Store::create(const std::filesystem::path& root, const AuthorityId& authority, Epoch epoch,
                            const Timestamp& at) {
  if (authority.empty()) {
    return Result<Store>::failure(
        Status::failure(ErrorCode::InvalidArgument, "store creation requires an authority identity", "store"));
  }
  if (epoch.is_zero()) {
    return Result<Store>::failure(
        Status::failure(ErrorCode::InvalidArgument, "store creation requires a non-zero control epoch", "store"));
  }

  const auto exists = platform::path_exists(root);
  if (!exists.ok()) {
    return Result<Store>::failure(exists.status());
  }
  if (exists.value()) {
    const auto reparse = platform::is_reparse_point(root);
    if (!reparse.ok()) {
      return Result<Store>::failure(reparse.status());
    }
    if (reparse.value()) {
      return Result<Store>::failure(Status::failure(
          ErrorCode::InvalidArgument,
          "store root '" + utf8(root) + "' is a reparse point; a redirected root is refused", "store"));
    }
    const auto directory = platform::is_directory(root);
    if (!directory.ok()) {
      return Result<Store>::failure(directory.status());
    }
    if (!directory.value()) {
      return Result<Store>::failure(Status::failure(
          ErrorCode::InvalidArgument, "store root '" + utf8(root) + "' exists and is not a directory", "store"));
    }
    for (const char* name : {kManifestFileName, kGuardFileName, kLockFileName, kRecordsDirectoryName,
                             kStagingDirectoryName}) {
      const auto present = platform::path_exists(root / name);
      if (!present.ok()) {
        return Result<Store>::failure(present.status());
      }
      if (present.value()) {
        return Result<Store>::failure(Status::failure(
            ErrorCode::AlreadyInitialized,
            "directory '" + utf8(root) + "' already contains store state ('" + name + "')", "store"));
      }
    }
    const auto files = platform::list_files(root);
    if (!files.ok()) {
      return Result<Store>::failure(files.status());
    }
    if (!files.value().empty()) {
      return Result<Store>::failure(Status::failure(
          ErrorCode::InvalidArgument,
          "directory '" + utf8(root) + "' is not empty and is not a store", "store"));
    }
  } else {
    const auto created = platform::create_directories(root);
    if (!created.ok()) {
      return Result<Store>::failure(created.status());
    }
  }

  Store store;
  store.root_ = root;
  store.info_.root = root;
  store.info_.mode = StoreMode::ReadWrite;
  store.info_.authority = authority;
  store.info_.epoch = epoch;
  store.info_.lock_held_exclusive = true;

  auto lock = platform::LockFile::acquire(root / kLockFileName, platform::LockMode::Exclusive);
  if (!lock.ok()) {
    return Result<Store>::failure(lock.status());
  }
  store.lock_ = lock.take();

  const auto records_directory = platform::create_directories(root / kRecordsDirectoryName);
  if (!records_directory.ok()) {
    return Result<Store>::failure(records_directory.status());
  }
  const auto staging_directory = platform::create_directories(root / kStagingDirectoryName);
  if (!staging_directory.ok()) {
    return Result<Store>::failure(staging_directory.status());
  }

  store.registry_.set_authority(authority);
  store.registry_.set_epoch(epoch);

  const auto committed = store.commit_locked(at);
  if (!committed.ok()) {
    return Result<Store>::failure(committed.status());
  }
  return Result<Store>::success(std::move(store));
}

Result<Store> Store::open(const std::filesystem::path& root, StoreMode mode) {
  Store store;
  store.root_ = root;
  store.info_.root = root;
  store.info_.mode = mode;

  const auto directory = platform::is_directory(root);
  if (!directory.ok()) {
    return Result<Store>::failure(directory.status());
  }
  if (!directory.value()) {
    return Result<Store>::failure(Status::failure(
        ErrorCode::StoreNotInitialized, "store root '" + utf8(root) + "' does not exist", "store"));
  }
  const auto reparse = platform::is_reparse_point(root);
  if (!reparse.ok()) {
    return Result<Store>::failure(reparse.status());
  }
  if (reparse.value()) {
    return Result<Store>::failure(Status::failure(
        ErrorCode::InvalidArgument,
        "store root '" + utf8(root) + "' is a reparse point; a redirected root is refused", "store"));
  }

  const auto lock_path = root / kLockFileName;
  const auto lock_exists = platform::path_exists(lock_path);
  if (!lock_exists.ok()) {
    return Result<Store>::failure(lock_exists.status());
  }
  if (!lock_exists.value()) {
    const auto manifest_exists = platform::path_exists(root / kManifestFileName);
    if (!manifest_exists.ok()) {
      return Result<Store>::failure(manifest_exists.status());
    }
    if (manifest_exists.value()) {
      return Result<Store>::failure(Status::failure(
          ErrorCode::CorruptStore, "the store lock file is missing while a manifest exists", "store"));
    }
    return Result<Store>::failure(Status::failure(
        ErrorCode::StoreNotInitialized, "directory '" + utf8(root) + "' is not an initialized store", "store"));
  }
  const auto lock_reparse = platform::is_reparse_point(lock_path);
  if (!lock_reparse.ok()) {
    return Result<Store>::failure(lock_reparse.status());
  }
  if (lock_reparse.value()) {
    return Result<Store>::failure(Status::failure(
        ErrorCode::InvalidArgument, "the store lock file is a reparse point", "store"));
  }

  auto lock = platform::LockFile::acquire(
      lock_path, mode == StoreMode::ReadWrite ? platform::LockMode::Exclusive : platform::LockMode::Shared);
  if (!lock.ok()) {
    return Result<Store>::failure(lock.status());
  }
  store.lock_ = lock.take();
  store.info_.lock_held_exclusive = mode == StoreMode::ReadWrite;

  const Status loaded = store.load_locked(mode);
  if (!loaded.ok()) {
    return Result<Store>::failure(loaded);
  }
  return Result<Store>::success(std::move(store));
}

Status Store::load_locked(StoreMode mode) {
  const auto manifest_path = root_ / kManifestFileName;
  const auto guard_path = root_ / kGuardFileName;
  const auto records_path = root_ / kRecordsDirectoryName;
  const auto staging_path = root_ / kStagingDirectoryName;

  const auto manifest_exists = platform::path_exists(manifest_path);
  if (!manifest_exists.ok()) {
    return manifest_exists.status();
  }
  const auto guard_exists = platform::path_exists(guard_path);
  if (!guard_exists.ok()) {
    return guard_exists.status();
  }
  if (!manifest_exists.value()) {
    if (guard_exists.value()) {
      return Status::failure(
          ErrorCode::CorruptStore,
          "the guard fence exists while the manifest is missing; refusing to start from partial durable state",
          "store");
    }
    return Status::failure(ErrorCode::StoreNotInitialized, "no manifest found in '" + utf8(root_) + "'", "store");
  }
  if (!guard_exists.value()) {
    return Status::failure(ErrorCode::CorruptStore,
                           "the guard fence is missing while a manifest exists; refusing to start", "store");
  }
  const auto records_directory = platform::is_directory(records_path);
  if (!records_directory.ok()) {
    return records_directory.status();
  }
  if (!records_directory.value()) {
    return Status::failure(ErrorCode::CorruptStore, "the records directory is missing", "store");
  }

  const auto manifest_bytes = platform::read_file(manifest_path, kMaxManifestFileBytes);
  if (!manifest_bytes.ok()) {
    return manifest_bytes.status();
  }
  const auto manifest_frame = decode_frame(manifest_bytes.value().data(), manifest_bytes.value().size(),
                                           FrameKind::StoreManifest, kMaxManifestPayloadBytes);
  if (!manifest_frame.ok()) {
    return manifest_frame.status();
  }
  const Digest manifest_digest = Digest::of(manifest_bytes.value().data(), manifest_bytes.value().size());
  const auto data = decode_manifest_payload(manifest_frame.value().payload);
  if (!data.ok()) {
    return data.status();
  }

  const auto guard_bytes = platform::read_file(guard_path, kMaxGuardFileBytes);
  if (!guard_bytes.ok()) {
    return guard_bytes.status();
  }
  const auto guard_frame = decode_frame(guard_bytes.value().data(), guard_bytes.value().size(), FrameKind::StoreGuard,
                                        kMaxGuardPayloadBytes);
  if (!guard_frame.ok()) {
    return guard_frame.status();
  }
  const auto guard = decode_guard_payload(guard_frame.value().payload);
  if (!guard.ok()) {
    return guard.status();
  }

  if (data.value().sequence < guard.value().sequence) {
    return Status::failure(
        ErrorCode::RollbackDetected,
        "the manifest is at sequence " + std::to_string(data.value().sequence.value()) +
            " but the guard fence records sequence " + std::to_string(guard.value().sequence.value()) +
            "; a stale copy of durable state was restored",
        "store");
  }
  if (data.value().sequence == guard.value().sequence && guard.value().manifest_digest != manifest_digest) {
    return Status::failure(ErrorCode::CorruptStore,
                           "the guard fence records manifest digest " + guard.value().manifest_digest.hex() +
                               " for sequence " + std::to_string(data.value().sequence.value()) +
                               " but the manifest hashes to " + manifest_digest.hex(),
                           "store");
  }
  if (guard.value().epoch > data.value().epoch) {
    return Status::failure(
        ErrorCode::RollbackDetected,
        "the manifest control epoch " + std::to_string(data.value().epoch.value()) +
            " is older than the guard fence epoch " + std::to_string(guard.value().epoch.value()) +
            "; a stale copy of durable state was restored",
        "store");
  }

  const bool guard_behind = guard.value().sequence < data.value().sequence || guard.value().epoch < data.value().epoch;
  if (guard_behind && mode == StoreMode::ReadWrite) {
    GuardRecord fresh;
    fresh.sequence = data.value().sequence;
    fresh.manifest_digest = manifest_digest;
    fresh.epoch = data.value().epoch;
    const auto payload = encode_guard_payload(fresh);
    if (!payload.ok()) {
      return payload.status();
    }
    const auto frame = encode_frame(FrameKind::StoreGuard, payload.value(), kMaxGuardPayloadBytes);
    if (!frame.ok()) {
      return frame.status();
    }
    const auto staged = staging_path / (std::string(kGuardFileName) + kStagingSuffix);
    const auto removal = platform::create_directories(staging_path);
    if (!removal.ok()) {
      return removal.status();
    }
    const auto written = platform::write_file_staged(staged, frame.value());
    if (!written.ok()) {
      return written.status();
    }
    const auto readback = platform::read_file(staged, kMaxGuardFileBytes);
    if (!readback.ok() || readback.value() != frame.value()) {
      return Status::failure(ErrorCode::CorruptStore,
                             "the repaired guard fence did not verify after staging", "store");
    }
    const auto published = platform::publish_staged(staged, guard_path, true);
    if (!published.ok()) {
      return published.status();
    }
    info_.guard_repaired = true;
  }

  registry_ = Registry{};
  registry_.set_authority(data.value().authority);
  registry_.set_epoch(data.value().epoch);

  for (const ClassRecord& record : data.value().records) {
    std::vector<StoredRevision> revisions;
    revisions.reserve(record.history.size());
    for (const ClassHistoryEntry& entry : record.history) {
      const auto path = records_path / (hex_of(entry.reference.digest) + kRecordSuffix);
      const auto bytes = platform::read_file(path, kMaxRecordFileBytes);
      if (!bytes.ok()) {
        Status failure = bytes.status();
        failure.add_secondary(Diagnostic{ErrorCode::CorruptStore, Severity::Refusal, record.class_id.str(),
                                         "referenced revision record '" + utf8(path) + "' cannot be read"});
        return Status::failure(ErrorCode::CorruptStore,
                               "a referenced revision record is missing or unreadable: " + failure.detail(),
                               record.class_id.str());
      }
      const Digest actual = Digest::of(bytes.value().data(), bytes.value().size());
      if (actual != entry.reference.digest) {
        return Status::failure(ErrorCode::CorruptStore,
                               "revision record '" + utf8(path) + "' hashes to " + actual.hex() +
                                   ", which is not its name",
                               record.class_id.str());
      }
      const auto content = decode_revision_frame(bytes.value().data(), bytes.value().size());
      if (!content.ok()) {
        return Status::failure(content.status().code(),
                               "revision record '" + utf8(path) + "' does not decode: " + content.status().detail(),
                               record.class_id.str());
      }
      if (content.value().class_id != record.class_id ||
          content.value().generation != entry.reference.generation ||
          content.value().revision != entry.reference.revision) {
        return Status::failure(ErrorCode::CorruptStore,
                               "revision record '" + utf8(path) + "' holds " + content.value().class_id.str() + " " +
                                   std::to_string(content.value().generation.value()) + "." +
                                   std::to_string(content.value().revision.value()) +
                                   ", which is not the identity recorded in the manifest",
                               record.class_id.str());
      }
      StoredRevision stored;
      stored.content = content.value();
      stored.digest = entry.reference.digest;
      stored.published_at = entry.published_at;
      revisions.push_back(std::move(stored));
    }
    const Status restored = registry_.restore_class(record, revisions);
    if (!restored.ok()) {
      return restored;
    }
  }

  const Status validated = registry_.validate_all();
  if (!validated.ok()) {
    return validated;
  }

  if (mode == StoreMode::ReadWrite) {
    const auto removed = remove_staging_files();
    if (!removed.ok()) {
      return removed.status();
    }
    info_.staging_files_removed = removed.value();
  }

  const ManifestData loaded_data = data.value();
  const auto index_digest = index_digest_of(loaded_data);
  if (!index_digest.ok()) {
    return index_digest.status();
  }

  info_.authority = loaded_data.authority;
  info_.epoch = loaded_data.epoch;
  info_.sequence = loaded_data.sequence;
  info_.manifest_digest = manifest_digest;
  info_.index_digest = index_digest.value();
  info_.written_at = loaded_data.written_at;
  info_.class_count = registry_.class_count();
  info_.revision_count = registry_.revision_count();

  const auto files = record_files();
  if (!files.ok()) {
    return files.status();
  }
  info_.record_files = files.value().size();
  std::set<Digest> referenced;
  for (const ClassRecord& record : registry_.class_records()) {
    for (const ClassHistoryEntry& entry : record.history) {
      referenced.insert(entry.reference.digest);
    }
  }
  std::size_t orphans = 0;
  for (const std::filesystem::path& path : files.value()) {
    Digest digest;
    if (!parse_record_file_stem(path, &digest)) {
      continue;
    }
    if (referenced.find(digest) == referenced.end()) {
      ++orphans;
    }
  }
  info_.orphan_records = orphans;
  manifest_frame_ = manifest_bytes.value();
  return Status::success();
}

Result<std::size_t> Store::remove_staging_files() {
  const auto staging_path = root_ / kStagingDirectoryName;
  const auto directory = platform::is_directory(staging_path);
  if (!directory.ok()) {
    return Result<std::size_t>::failure(directory.status());
  }
  if (!directory.value()) {
    const auto created = platform::create_directories(staging_path);
    if (!created.ok()) {
      return Result<std::size_t>::failure(created.status());
    }
    return Result<std::size_t>::success(0);
  }
  return platform::remove_files_in(staging_path);
}

Result<std::vector<std::filesystem::path>> Store::record_files() const {
  return platform::list_files(root_ / kRecordsDirectoryName);
}

Result<std::size_t> Store::ensure_records(const std::vector<ClassRecord>& records) {
  std::size_t written = 0;
  std::vector<std::filesystem::path> staged;
  std::vector<std::filesystem::path> targets;
  const auto records_path = root_ / kRecordsDirectoryName;
  const auto staging_path = root_ / kStagingDirectoryName;

  for (const ClassRecord& record : records) {
    const auto history = registry_.stored_history(record.class_id);
    if (!history.ok()) {
      return Result<std::size_t>::failure(history.status());
    }
    for (const StoredRevision& stored : history.value()) {
      const auto target = records_path / (hex_of(stored.digest) + kRecordSuffix);
      const auto present = platform::path_exists(target);
      if (!present.ok()) {
        return Result<std::size_t>::failure(present.status());
      }
      if (present.value()) {
        // Existing record files were verified in full when the store was
        // opened, and every open re-verifies them, so the commit path only
        // checks that they are still present. A file that disappeared or was
        // corrupted behind the store's back is still refused by the next open
        // and by "scr store verify"; skipping the re-read here keeps the cost
        // of a commit linear in the number of new revisions rather than in the
        // size of the whole history.
        continue;
      }
      const auto frame = encode_revision_frame(stored.content);
      if (!frame.ok()) {
        return Result<std::size_t>::failure(frame.status());
      }
      if (Digest::of(frame.value().data(), frame.value().size()) != stored.digest) {
        return Result<std::size_t>::failure(Status::failure(
            ErrorCode::Internal, "re-encoded revision frame does not match its digest", record.class_id.str()));
      }
      const auto staged_path = staging_path / (hex_of(stored.digest) + std::string(kRecordSuffix) + kStagingSuffix);
      const auto written_file = platform::write_file_staged(staged_path, frame.value());
      if (!written_file.ok()) {
        return Result<std::size_t>::failure(written_file.status());
      }
      const auto readback = platform::read_file(staged_path, kMaxRecordFileBytes);
      if (!readback.ok() || readback.value() != frame.value()) {
        return Result<std::size_t>::failure(Status::failure(
            ErrorCode::CorruptStore,
            "staged revision record '" + utf8(staged_path) + "' did not verify", record.class_id.str()));
      }
      staged.push_back(staged_path);
      targets.push_back(target);
    }
  }

  fault_point("records-staged");

  for (std::size_t index = 0; index < staged.size(); ++index) {
    const auto published = platform::publish_staged(staged[index], targets[index], false);
    if (!published.ok()) {
      return Result<std::size_t>::failure(published.status());
    }
    const auto readback = platform::read_file(targets[index], kMaxRecordFileBytes);
    if (!readback.ok()) {
      return Result<std::size_t>::failure(readback.status());
    }
    if (Digest::of(readback.value().data(), readback.value().size()) !=
        Digest::parse(targets[index].stem().string()).value()) {
      return Result<std::size_t>::failure(Status::failure(
          ErrorCode::CorruptStore,
          "published revision record '" + utf8(targets[index]) + "' does not hash to its name", "store"));
    }
    ++written;
  }
  fault_point("records-published");
  return Result<std::size_t>::success(written);
}

Result<CommitOutcome> Store::commit_locked(const Timestamp& at) {
  if (info_.mode != StoreMode::ReadWrite) {
    return Result<CommitOutcome>::failure(Status::failure(
        ErrorCode::UnauthorizedTransition,
        "the store was opened read only; publication requires the exclusive writer", "store"));
  }
  if (registry_.authority() != info_.authority) {
    return Result<CommitOutcome>::failure(Status::failure(
        ErrorCode::UnauthorizedTransition, "the in-memory authority does not match the store authority", "store"));
  }
  if (registry_.epoch() < info_.epoch) {
    return Result<CommitOutcome>::failure(Status::failure(
        ErrorCode::StaleAuthority,
        "the in-memory control epoch " + std::to_string(registry_.epoch().value()) +
            " is older than the authoritative epoch " + std::to_string(info_.epoch.value()),
        "store"));
  }

  ManifestData data;
  data.authority = info_.authority;
  data.epoch = registry_.epoch();
  data.sequence = info_.sequence;
  data.written_at = at;
  data.records = registry_.class_records();

  const auto index_digest = index_digest_of(data);
  if (!index_digest.ok()) {
    return Result<CommitOutcome>::failure(index_digest.status());
  }

  CommitOutcome outcome;
  outcome.sequence = info_.sequence;
  outcome.manifest_digest = info_.manifest_digest;
  if (index_digest.value() == info_.index_digest) {
    outcome.idempotent = true;
    return Result<CommitOutcome>::success(std::move(outcome));
  }

  const auto next_sequence = info_.sequence.next();
  if (!next_sequence.ok()) {
    return Result<CommitOutcome>::failure(next_sequence.status());
  }
  data.sequence = next_sequence.value();

  const auto payload = encode_manifest_payload(data);
  if (!payload.ok()) {
    return Result<CommitOutcome>::failure(payload.status());
  }
  const auto frame = encode_frame(FrameKind::StoreManifest, payload.value(), kMaxManifestPayloadBytes);
  if (!frame.ok()) {
    return Result<CommitOutcome>::failure(frame.status());
  }
  const Digest frame_digest = Digest::of(frame.value().data(), frame.value().size());

  const auto records_written = ensure_records(data.records);
  if (!records_written.ok()) {
    return Result<CommitOutcome>::failure(records_written.status());
  }
  outcome.records_written = records_written.value();

  const auto staging_path = root_ / kStagingDirectoryName;
  const auto manifest_path = root_ / kManifestFileName;
  const auto guard_path = root_ / kGuardFileName;
  const auto staged_manifest = staging_path / (std::string(kManifestFileName) + kStagingSuffix);

  const auto staged = platform::write_file_staged(staged_manifest, frame.value());
  if (!staged.ok()) {
    return Result<CommitOutcome>::failure(staged.status());
  }
  const auto staged_readback = platform::read_file(staged_manifest, kMaxManifestFileBytes);
  if (!staged_readback.ok() || staged_readback.value() != frame.value()) {
    return Result<CommitOutcome>::failure(Status::failure(
        ErrorCode::CorruptStore, "the staged manifest did not verify before publication", "store"));
  }
  fault_point("manifest-staged");

  const auto published = platform::publish_staged(staged_manifest, manifest_path, true);
  if (!published.ok()) {
    return Result<CommitOutcome>::failure(published.status());
  }
  const auto published_readback = platform::read_file(manifest_path, kMaxManifestFileBytes);
  if (!published_readback.ok() || published_readback.value() != frame.value()) {
    return Result<CommitOutcome>::failure(Status::failure(
        ErrorCode::CorruptStore, "the published manifest did not verify after the commit point", "store"));
  }
  fault_point("manifest-published");

  GuardRecord guard;
  guard.sequence = next_sequence.value();
  guard.manifest_digest = frame_digest;
  guard.epoch = data.epoch;
  const auto guard_payload = encode_guard_payload(guard);
  if (!guard_payload.ok()) {
    return Result<CommitOutcome>::failure(guard_payload.status());
  }
  const auto guard_frame = encode_frame(FrameKind::StoreGuard, guard_payload.value(), kMaxGuardPayloadBytes);
  if (!guard_frame.ok()) {
    return Result<CommitOutcome>::failure(guard_frame.status());
  }
  const auto staged_guard = staging_path / (std::string(kGuardFileName) + kStagingSuffix);
  const auto guard_staged = platform::write_file_staged(staged_guard, guard_frame.value());
  if (!guard_staged.ok()) {
    return Result<CommitOutcome>::failure(guard_staged.status());
  }
  const auto guard_readback = platform::read_file(staged_guard, kMaxGuardFileBytes);
  if (!guard_readback.ok() || guard_readback.value() != guard_frame.value()) {
    return Result<CommitOutcome>::failure(Status::failure(
        ErrorCode::CorruptStore, "the staged guard fence did not verify before publication", "store"));
  }
  fault_point("guard-staged");
  const auto guard_published = platform::publish_staged(staged_guard, guard_path, true);
  if (!guard_published.ok()) {
    return Result<CommitOutcome>::failure(guard_published.status());
  }
  fault_point("guard-published");

  manifest_frame_ = frame.value();
  info_.sequence = next_sequence.value();
  info_.manifest_digest = frame_digest;
  info_.index_digest = index_digest.value();
  info_.written_at = at;
  info_.epoch = data.epoch;
  info_.class_count = registry_.class_count();
  info_.revision_count = registry_.revision_count();
  const auto files = record_files();
  if (files.ok()) {
    info_.record_files = files.value().size();
  }

  outcome.sequence = next_sequence.value();
  outcome.manifest_digest = frame_digest;
  return Result<CommitOutcome>::success(std::move(outcome));
}

Result<CommitOutcome> Store::commit(const Timestamp& at) {
  return commit_locked(at);
}

Result<CommitOutcome> Store::adopt_epoch(Epoch epoch, const Timestamp& at) {
  if (info_.mode != StoreMode::ReadWrite) {
    return Result<CommitOutcome>::failure(Status::failure(
        ErrorCode::UnauthorizedTransition, "adopting a control epoch requires the exclusive writer", "store"));
  }
  if (epoch <= info_.epoch) {
    return Result<CommitOutcome>::failure(Status::failure(
        ErrorCode::InvalidArgument,
        "a new control epoch must be strictly greater than the current epoch " +
            std::to_string(info_.epoch.value()),
        "store"));
  }
  registry_.set_epoch(epoch);
  const auto committed = commit_locked(at);
  if (!committed.ok()) {
    registry_.set_epoch(info_.epoch);
    return Result<CommitOutcome>::failure(committed.status());
  }
  return committed;
}

Result<StorePublishOutcome> Store::publish(const ClassRevision& content, const PublishOptions& options) {
  if (info_.mode != StoreMode::ReadWrite) {
    return Result<StorePublishOutcome>::failure(Status::failure(
        ErrorCode::UnauthorizedTransition, "the store was opened read only", "store"));
  }
  const auto snapshot = registry_.snapshot_class(content.class_id);
  if (!snapshot.ok()) {
    return Result<StorePublishOutcome>::failure(snapshot.status());
  }
  auto published = registry_.publish(content, options);
  if (!published.ok()) {
    return Result<StorePublishOutcome>::failure(published.status());
  }
  auto committed = commit_locked(options.at);
  if (!committed.ok()) {
    const Status rollback = registry_.restore_snapshot(snapshot.value());
    Status failure = committed.status();
    Diagnostic note;
    note.code = rollback.ok() ? ErrorCode::Internal : rollback.code();
    note.severity = Severity::Info;
    note.subject = content.class_id.str();
    note.detail = rollback.ok() ? "the in-memory publication was rolled back because the durable commit failed"
                                : "the in-memory rollback itself failed: " + rollback.to_string();
    failure.add_secondary(std::move(note));
    return Result<StorePublishOutcome>::failure(failure);
  }
  StorePublishOutcome outcome;
  outcome.publish = published.take();
  outcome.commit = committed.take();
  return Result<StorePublishOutcome>::success(std::move(outcome));
}

Result<StoreRetireOutcome> Store::retire(const ClassId& class_id, const BoundedText& reason, const Timestamp& at) {
  if (info_.mode != StoreMode::ReadWrite) {
    return Result<StoreRetireOutcome>::failure(Status::failure(
        ErrorCode::UnauthorizedTransition, "the store was opened read only", "store"));
  }
  const auto snapshot = registry_.snapshot_class(class_id);
  if (!snapshot.ok()) {
    return Result<StoreRetireOutcome>::failure(snapshot.status());
  }
  auto retired = registry_.retire(class_id, reason, at);
  if (!retired.ok()) {
    return Result<StoreRetireOutcome>::failure(retired.status());
  }
  auto committed = commit_locked(at);
  if (!committed.ok()) {
    const Status rollback = registry_.restore_snapshot(snapshot.value());
    Status failure = committed.status();
    Diagnostic note;
    note.code = rollback.ok() ? ErrorCode::Internal : rollback.code();
    note.severity = Severity::Info;
    note.subject = class_id.str();
    note.detail = rollback.ok() ? "the in-memory retirement was rolled back because the durable commit failed"
                                : "the in-memory rollback itself failed: " + rollback.to_string();
    failure.add_secondary(std::move(note));
    return Result<StoreRetireOutcome>::failure(failure);
  }
  StoreRetireOutcome outcome;
  outcome.retire = retired.take();
  outcome.commit = committed.take();
  return Result<StoreRetireOutcome>::success(std::move(outcome));
}

Result<StoreVerifyReport> Store::verify() const {
  StoreVerifyReport report;
  report.sequence = info_.sequence;
  report.manifest_digest = info_.manifest_digest;

  const auto manifest_path = root_ / kManifestFileName;
  const auto manifest_bytes = platform::read_file(manifest_path, kMaxManifestFileBytes);
  if (!manifest_bytes.ok()) {
    report.problems.push_back("the manifest cannot be read: " + manifest_bytes.status().to_string());
    report.ok = false;
    return Result<StoreVerifyReport>::success(std::move(report));
  }
  const Digest manifest_digest = Digest::of(manifest_bytes.value().data(), manifest_bytes.value().size());
  if (manifest_digest != info_.manifest_digest) {
    report.problems.push_back("the manifest digest " + manifest_digest.hex() +
                              " does not match the digest recorded at open time " + info_.manifest_digest.hex());
  }
  const auto manifest_frame = decode_frame(manifest_bytes.value().data(), manifest_bytes.value().size(),
                                           FrameKind::StoreManifest, kMaxManifestPayloadBytes);
  if (!manifest_frame.ok()) {
    report.problems.push_back("the manifest frame is invalid: " + manifest_frame.status().to_string());
  } else {
    const auto data = decode_manifest_payload(manifest_frame.value().payload);
    if (!data.ok()) {
      report.problems.push_back("the manifest payload is invalid: " + data.status().to_string());
    }
  }

  const auto guard_path = root_ / kGuardFileName;
  const auto guard_bytes = platform::read_file(guard_path, kMaxGuardFileBytes);
  if (!guard_bytes.ok()) {
    report.problems.push_back("the guard fence cannot be read: " + guard_bytes.status().to_string());
  } else {
    const auto guard_frame = decode_frame(guard_bytes.value().data(), guard_bytes.value().size(),
                                          FrameKind::StoreGuard, kMaxGuardPayloadBytes);
    if (!guard_frame.ok()) {
      report.problems.push_back("the guard fence frame is invalid: " + guard_frame.status().to_string());
    } else {
      const auto guard = decode_guard_payload(guard_frame.value().payload);
      if (!guard.ok()) {
        report.problems.push_back("the guard fence payload is invalid: " + guard.status().to_string());
      } else {
        if (guard.value().sequence > info_.sequence) {
          report.problems.push_back("the guard fence sequence " +
                                    std::to_string(guard.value().sequence.value()) +
                                    " is ahead of the manifest sequence " + std::to_string(info_.sequence.value()));
        }
        if (guard.value().epoch > info_.epoch) {
          report.problems.push_back("the guard fence epoch " + std::to_string(guard.value().epoch.value()) +
                                    " is ahead of the manifest epoch " + std::to_string(info_.epoch.value()));
        }
      }
    }
  }

  const auto records_path = root_ / kRecordsDirectoryName;
  std::set<Digest> referenced;
  for (const ClassRecord& record : registry_.class_records()) {
    for (const ClassHistoryEntry& entry : record.history) {
      referenced.insert(entry.reference.digest);
      ++report.referenced_records;
      const auto path = records_path / (hex_of(entry.reference.digest) + kRecordSuffix);
      const auto bytes = platform::read_file(path, kMaxRecordFileBytes);
      if (!bytes.ok()) {
        report.problems.push_back("referenced record '" + utf8(path) + "' cannot be read: " +
                                  bytes.status().to_string());
        continue;
      }
      const Digest actual = Digest::of(bytes.value().data(), bytes.value().size());
      if (actual != entry.reference.digest) {
        report.problems.push_back("referenced record '" + utf8(path) + "' hashes to " + actual.hex());
        continue;
      }
      const auto content = decode_revision_frame(bytes.value().data(), bytes.value().size());
      if (!content.ok()) {
        report.problems.push_back("referenced record '" + utf8(path) + "' does not decode: " +
                                  content.status().to_string());
        continue;
      }
      ++report.verified_records;
    }
  }

  const auto files = record_files();
  if (!files.ok()) {
    report.problems.push_back("the records directory cannot be enumerated: " + files.status().to_string());
  } else {
    for (const std::filesystem::path& path : files.value()) {
      Digest digest;
      if (!parse_record_file_stem(path, &digest)) {
        report.problems.push_back("unrecognized file in the records directory: '" + utf8(path) + "'");
        continue;
      }
      if (referenced.find(digest) == referenced.end()) {
        ++report.orphan_records;
      }
    }
  }

  const auto staging_path = root_ / kStagingDirectoryName;
  const auto staging_exists = platform::is_directory(staging_path);
  if (!staging_exists.ok()) {
    report.problems.push_back("the staging directory cannot be inspected: " + staging_exists.status().to_string());
  } else if (!staging_exists.value()) {
    report.problems.push_back("the staging directory is missing: '" + utf8(staging_path) + "'");
  } else {
    const auto staging_files = platform::list_files(staging_path);
    if (!staging_files.ok()) {
      report.problems.push_back("the staging directory cannot be enumerated: " + staging_files.status().to_string());
    } else {
      report.staging_files = staging_files.value().size();
    }
  }

  report.ok = report.problems.empty();
  return Result<StoreVerifyReport>::success(std::move(report));
}

Result<std::size_t> Store::compact() {
  if (info_.mode != StoreMode::ReadWrite) {
    return Result<std::size_t>::failure(Status::failure(
        ErrorCode::UnauthorizedTransition, "compaction requires the exclusive writer", "store"));
  }
  const auto before = verify();
  if (!before.ok()) {
    return Result<std::size_t>::failure(before.status());
  }
  if (!before.value().ok) {
    return Result<std::size_t>::failure(Status::failure(
        ErrorCode::CorruptStore, "refusing to compact a store that does not verify", "store"));
  }

  std::size_t removed = 0;
  const auto staging_removed = remove_staging_files();
  if (!staging_removed.ok()) {
    return Result<std::size_t>::failure(staging_removed.status());
  }
  removed += staging_removed.value();

  std::set<Digest> referenced;
  for (const ClassRecord& record : registry_.class_records()) {
    for (const ClassHistoryEntry& entry : record.history) {
      referenced.insert(entry.reference.digest);
    }
  }

  const auto files = record_files();
  if (!files.ok()) {
    return Result<std::size_t>::failure(files.status());
  }
  for (const std::filesystem::path& path : files.value()) {
    Digest digest;
    if (!parse_record_file_stem(path, &digest)) {
      // Never delete a file whose name this store does not understand.
      continue;
    }
    if (referenced.find(digest) != referenced.end()) {
      continue;
    }
    const auto deleted = platform::remove_file(path);
    if (!deleted.ok()) {
      return Result<std::size_t>::failure(deleted.status());
    }
    ++removed;
  }

  const auto after = verify();
  if (!after.ok()) {
    return Result<std::size_t>::failure(after.status());
  }
  if (!after.value().ok) {
    return Result<std::size_t>::failure(Status::failure(
        ErrorCode::Internal, "compaction produced a state that no longer verifies", "store"));
  }
  info_.orphan_records = 0;
  if (info_.record_files >= removed) {
    info_.record_files -= removed;
  }
  return Result<std::size_t>::success(removed);
}

}  // namespace scr
