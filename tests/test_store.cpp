#include "harness.hpp"

#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "scr/bytes.hpp"
#include "scr/canonical.hpp"
#include "scr/platform.hpp"
#include "scr/text.hpp"

using namespace scr;

namespace {

const char* kAuthority = "scr-test";

struct Fixture {
  explicit Fixture(const std::string& label) : directory(label) {}

  scrtest::TempDirectory directory;
  std::filesystem::path root() const { return directory.path() / "store"; }
};

Result<Store> create_store(const Fixture& fixture, const std::string& authority = kAuthority) {
  const auto parsed = AuthorityId::parse(authority);
  const auto at = Timestamp::from_unix_seconds(1700000000);
  return Store::create(fixture.root(), parsed.value(), Epoch::from(1), at.value());
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  const auto bytes = platform::read_file(path, 1u << 20);
  return bytes.ok() ? bytes.value() : std::vector<std::uint8_t>{};
}

bool write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  if (!std::filesystem::exists(path.parent_path())) {
    if (!platform::create_directories(path.parent_path()).ok()) {
      return false;
    }
  }
  const auto staged = platform::path_from_utf8(path.string() + ".teststage");
  if (!staged.ok()) {
    return false;
  }
  if (!platform::write_file_staged(staged.value(), bytes).ok()) {
    return false;
  }
  return platform::publish_staged(staged.value(), path, true).ok();
}

ClassRevision next_revision(const Registry& registry, const std::string& class_id, std::uint64_t ppm) {
  return scrtest::make_published_revision(registry, class_id, ppm);
}

std::size_t count_files(const std::filesystem::path& directory) {
  const auto files = platform::list_files(directory);
  return files.ok() ? files.value().size() : 0u;
}

}  // namespace

SCR_TEST(store_publish_reopen_round_trip) {
  Fixture fixture("store-roundtrip");
  auto store = create_store(fixture);
  SCR_REQUIRE(context, store.ok());
  SCR_CHECK_EQ(context, store.value().info().sequence.value(), static_cast<std::uint64_t>(1));
  SCR_CHECK_EQ(context, store.value().info().class_count, std::size_t{0});

  const ClassRevision genesis = scrtest::make_simple_revision("store.core/hall", 1, 1, "genesis", 999000);
  const auto published = store.value().publish(genesis, PublishOptions{});
  SCR_REQUIRE(context, published.ok());
  SCR_CHECK_EQ(context, published.value().commit.sequence.value(), static_cast<std::uint64_t>(2));
  SCR_CHECK_EQ(context, published.value().commit.records_written, std::size_t{1});

  const ClassRevision second = next_revision(store.value().registry(), "store.core/hall", 999900);
  SCR_REQUIRE(context, store.value().publish(second, PublishOptions{}).ok());
  const ClassRevision third = next_revision(store.value().registry(), "store.core/hall", 999990);
  SCR_REQUIRE(context, store.value().publish(third, PublishOptions{}).ok());

  const auto before = store.value().registry().class_record(ClassId::parse("store.core/hall").value());
  SCR_REQUIRE(context, before.ok());
  const Sequence sequence_before = store.value().info().sequence;
  const Digest index_digest = store.value().info().index_digest;
  store.value() = Store{};

  auto reopened = Store::open(fixture.root(), StoreMode::ReadWrite);
  SCR_REQUIRE(context, reopened.ok());
  SCR_CHECK_EQ(context, reopened.value().info().sequence.value(), sequence_before.value());
  SCR_CHECK_EQ(context, reopened.value().info().index_digest.hex(), index_digest.hex());
  SCR_CHECK_EQ(context, reopened.value().info().class_count, std::size_t{1});
  SCR_CHECK_EQ(context, reopened.value().info().revision_count, std::size_t{3});
  SCR_CHECK_EQ(context, reopened.value().info().orphan_records, std::size_t{0});

  const auto after = reopened.value().registry().class_record(ClassId::parse("store.core/hall").value());
  SCR_REQUIRE(context, after.ok());
  SCR_CHECK_EQ(context, after.value().tip.value(), static_cast<std::uint32_t>(3));
  SCR_CHECK_EQ(context, after.value().tip_digest.hex(), before.value().history.back().reference.digest.hex());
  SCR_CHECK_EQ(context, after.value().history.size(), std::size_t{3});
  SCR_CHECK(context, reopened.value().registry().validate_all().ok());

  // A fresh open of the same store must produce exactly the same index digest.
  reopened.value() = Store{};
  auto again = Store::open(fixture.root(), StoreMode::ReadOnly);
  SCR_REQUIRE(context, again.ok());
  SCR_CHECK_EQ(context, again.value().info().index_digest.hex(), index_digest.hex());

  const auto report = again.value().verify();
  SCR_REQUIRE(context, report.ok());
  SCR_CHECK(context, report.value().ok);
  SCR_CHECK_EQ(context, report.value().verified_records, std::size_t{3});
  SCR_CHECK_EQ(context, report.value().orphan_records, std::size_t{0});
}

SCR_TEST(store_read_only_refuses_mutation) {
  Fixture fixture("store-readonly");
  auto store = create_store(fixture);
  SCR_REQUIRE(context, store.ok());
  SCR_REQUIRE(context, store.value().publish(scrtest::make_simple_revision("store.core/hall", 1, 1, "genesis", 999000),
                                             PublishOptions{})
                           .ok());
  store.value() = Store{};

  const Sequence sequence = Sequence::from(0);
  {
    auto read_only = Store::open(fixture.root(), StoreMode::ReadOnly);
    SCR_REQUIRE(context, read_only.ok());
    const ClassRevision candidate = scrtest::make_published_revision(read_only.value().registry(), "store.core/hall", 999900);
    const auto refused = read_only.value().publish(candidate, PublishOptions{});
    SCR_CHECK_EQ(context, refused.status().code(), ErrorCode::UnauthorizedTransition);
    SCR_CHECK_EQ(context, read_only.value().registry().class_record(ClassId::parse("store.core/hall").value())
                              .value()
                              .tip.value(),
                 static_cast<std::uint32_t>(1));
    const auto compact = read_only.value().compact();
    SCR_CHECK_EQ(context, compact.status().code(), ErrorCode::UnauthorizedTransition);
    SCR_CHECK_EQ(context, read_only.value().info().sequence.value(), sequence.value() + 2);
  }

  // The failed mutation left nothing behind: the store still verifies and the
  // sequence did not advance.
  auto verify_store = Store::open(fixture.root(), StoreMode::ReadOnly);
  SCR_REQUIRE(context, verify_store.ok());
  const auto report = verify_store.value().verify();
  SCR_REQUIRE(context, report.ok());
  SCR_CHECK(context, report.value().ok);
  SCR_CHECK_EQ(context, verify_store.value().info().sequence.value(), static_cast<std::uint64_t>(2));
}

SCR_TEST(store_corruption_is_detected_and_fails_closed) {
  Fixture fixture("store-corruption");
  auto store = create_store(fixture);
  SCR_REQUIRE(context, store.ok());
  SCR_REQUIRE(context, store.value().publish(scrtest::make_simple_revision("store.core/hall", 1, 1, "genesis", 999000),
                                             PublishOptions{})
                           .ok());
  const auto record_files = platform::list_files(fixture.root() / "records");
  SCR_REQUIRE(context, record_files.ok());
  SCR_REQUIRE(context, record_files.value().size() == 1);
  const std::filesystem::path record_path = record_files.value().front();
  const std::filesystem::path manifest_path = fixture.root() / "manifest";
  const std::filesystem::path guard_path = fixture.root() / "guard";

  const std::vector<std::uint8_t> manifest = read_bytes(manifest_path);
  const std::vector<std::uint8_t> guard = read_bytes(guard_path);
  const std::vector<std::uint8_t> record = read_bytes(record_path);
  SCR_REQUIRE(context, !manifest.empty());
  SCR_REQUIRE(context, !guard.empty());
  SCR_REQUIRE(context, !record.empty());
  store.value() = Store{};

  const auto flip = [](std::vector<std::uint8_t> bytes, std::size_t index) {
    bytes[index] = static_cast<std::uint8_t>(bytes[index] ^ 0x40u);
    return bytes;
  };

  struct Case {
    const char* name;
    std::function<bool()> prepare;
    ErrorCode expected;
  };

  const std::vector<Case> cases = {
      {"manifest bit flip", [&] { return write_bytes(manifest_path, flip(manifest, 20)); }, ErrorCode::CorruptStore},
      {"manifest truncation", [&] { return write_bytes(manifest_path, std::vector<std::uint8_t>(manifest.begin(), manifest.end() - 1)); }, ErrorCode::TruncatedInput},
      {"manifest trailing byte", [&] {
         std::vector<std::uint8_t> extended = manifest;
         extended.push_back(0);
         return write_bytes(manifest_path, extended);
       }, ErrorCode::TruncatedInput},
      {"manifest removed", [&] { return platform::remove_file(manifest_path).ok(); }, ErrorCode::CorruptStore},
      {"guard bit flip", [&] { return write_bytes(guard_path, flip(guard, 16)); }, ErrorCode::CorruptStore},
      {"guard removed", [&] { return platform::remove_file(guard_path).ok(); }, ErrorCode::CorruptStore},
      {"guard truncated", [&] { return write_bytes(guard_path, std::vector<std::uint8_t>(guard.begin(), guard.begin() + 8)); }, ErrorCode::TruncatedInput},
      {"record bit flip", [&] { return write_bytes(record_path, flip(record, 40)); }, ErrorCode::CorruptStore},
      {"record removed", [&] { return platform::remove_file(record_path).ok(); }, ErrorCode::CorruptStore},
      {"record truncated", [&] { return write_bytes(record_path, std::vector<std::uint8_t>(record.begin(), record.end() - 2)); }, ErrorCode::CorruptStore},
      {"lock removed", [&] { return platform::remove_file(fixture.root() / "LOCK").ok(); }, ErrorCode::CorruptStore},
      {"records directory removed", [&] { return std::filesystem::remove_all(fixture.root() / "records") > 0; }, ErrorCode::CorruptStore},
  };

  for (const Case& test_case : cases) {
    SCR_REQUIRE(context, write_bytes(manifest_path, manifest));
    SCR_REQUIRE(context, write_bytes(guard_path, guard));
    if (!std::filesystem::exists(record_path.parent_path())) {
      std::error_code ignored;
      std::filesystem::create_directories(record_path.parent_path(), ignored);
    }
    SCR_REQUIRE(context, write_bytes(record_path, record));
    if (!std::filesystem::exists(fixture.root() / "LOCK")) {
      std::ofstream create_lock(fixture.root() / "LOCK");
      create_lock << "test";
      create_lock.close();
    }
    SCR_REQUIRE(context, test_case.prepare());
    const auto opened = Store::open(fixture.root(), StoreMode::ReadOnly);
    if (opened.ok()) {
      context.fail(std::string("a corrupted store opened successfully: ") + test_case.name, __FILE__, __LINE__);
      continue;
    }
    if (opened.status().code() != test_case.expected) {
      context.fail(std::string("corruption case '") + test_case.name + "' produced " +
                       scr::to_string(opened.status().code()) + " instead of " + scr::to_string(test_case.expected) +
                       ": " + opened.status().detail(),
                   __FILE__, __LINE__);
    }
  }

  // Restore the original state and confirm the store opens again.
  SCR_REQUIRE(context, write_bytes(manifest_path, manifest));
  SCR_REQUIRE(context, write_bytes(guard_path, guard));
  SCR_REQUIRE(context, write_bytes(record_path, record));
  auto restored = Store::open(fixture.root(), StoreMode::ReadOnly);
  SCR_REQUIRE(context, restored.ok());
  SCR_CHECK(context, restored.value().verify().value().ok);
}

SCR_TEST(store_detects_rollback_and_repairs_the_guard) {
  Fixture fixture("store-rollback");
  auto store = create_store(fixture);
  SCR_REQUIRE(context, store.ok());

  const ClassRevision genesis = scrtest::make_simple_revision("store.core/hall", 1, 1, "genesis", 999000);
  SCR_REQUIRE(context, store.value().publish(genesis, PublishOptions{}).ok());
  const std::filesystem::path manifest_path = fixture.root() / "manifest";
  const std::filesystem::path guard_path = fixture.root() / "guard";
  const std::vector<std::uint8_t> manifest_after_first = read_bytes(manifest_path);
  const std::vector<std::uint8_t> guard_after_first = read_bytes(guard_path);

  const ClassRevision second = next_revision(store.value().registry(), "store.core/hall", 999900);
  SCR_REQUIRE(context, store.value().publish(second, PublishOptions{}).ok());
  const std::vector<std::uint8_t> manifest_after_second = read_bytes(manifest_path);
  store.value() = Store{};

  // (1) A manifest that is behind the guard fence is a restored stale copy.
  SCR_REQUIRE(context, write_bytes(manifest_path, manifest_after_first));
  const auto rolled_back = Store::open(fixture.root(), StoreMode::ReadOnly);
  SCR_REQUIRE(context, !rolled_back.ok());
  SCR_CHECK_EQ(context, rolled_back.status().code(), ErrorCode::RollbackDetected);

  // (2) A guard that is behind the manifest is the expected crash window: a
  // writable open repairs it, and a read-only open accepts the manifest as the
  // commit point.
  SCR_REQUIRE(context, write_bytes(manifest_path, manifest_after_second));
  SCR_REQUIRE(context, write_bytes(guard_path, guard_after_first));
  {
    auto read_only = Store::open(fixture.root(), StoreMode::ReadOnly);
    SCR_REQUIRE(context, read_only.ok());
    SCR_CHECK_EQ(context, read_only.value().info().sequence.value(), static_cast<std::uint64_t>(3));
    SCR_CHECK(context, !read_only.value().info().guard_repaired);
  }
  {
    auto writable = Store::open(fixture.root(), StoreMode::ReadWrite);
    SCR_REQUIRE(context, writable.ok());
    SCR_CHECK(context, writable.value().info().guard_repaired);
  }
  {
    auto reopened = Store::open(fixture.root(), StoreMode::ReadOnly);
    SCR_REQUIRE(context, reopened.ok());
    SCR_CHECK(context, !reopened.value().info().guard_repaired);
    SCR_CHECK_EQ(context, reopened.value().info().sequence.value(), static_cast<std::uint64_t>(3));
  }
}

SCR_TEST(store_compaction_removes_only_unreferenced_files) {
  Fixture fixture("store-compaction");
  auto store = create_store(fixture);
  SCR_REQUIRE(context, store.ok());
  SCR_REQUIRE(context, store.value().publish(scrtest::make_simple_revision("store.core/hall", 1, 1, "genesis", 999000),
                                             PublishOptions{})
                           .ok());

  const std::string orphan_name = std::string(64, 'a') + ".scr";
  const std::filesystem::path orphan = fixture.root() / "records" / orphan_name;
  SCR_REQUIRE(context, write_bytes(orphan, {1, 2, 3, 4}));
  const std::filesystem::path staging_residue = fixture.root() / "staging" / "manifest.tmp";
  SCR_REQUIRE(context, write_bytes(staging_residue, {9, 9, 9}));
  const std::filesystem::path foreign = fixture.root() / "records" / "notes.txt";

  const auto before = store.value().info().revision_count;
  const std::size_t removed = store.value().compact().value();
  SCR_CHECK(context, removed >= std::size_t{2});
  SCR_CHECK(context, !std::filesystem::exists(orphan));
  SCR_CHECK(context, !std::filesystem::exists(staging_residue));
  SCR_CHECK_EQ(context, store.value().info().revision_count, before);
  SCR_CHECK(context, store.value().registry().validate_all().ok());

  // The foreign file is reported by verification, and compaction then refuses
  // rather than deleting a file the store does not understand.
  SCR_REQUIRE(context, write_bytes(foreign, {7}));
  const auto report = store.value().verify();
  SCR_REQUIRE(context, report.ok());
  SCR_CHECK(context, !report.value().ok);
  const auto refused = store.value().compact();
  SCR_CHECK_EQ(context, refused.status().code(), ErrorCode::CorruptStore);
  SCR_CHECK(context, std::filesystem::exists(foreign));
  SCR_REQUIRE(context, platform::remove_file(foreign).ok());
  SCR_CHECK(context, store.value().verify().value().ok);
}

SCR_TEST(store_rejects_hostile_manifests_and_paths) {
  Fixture fixture("store-hostile");
  auto store = create_store(fixture);
  SCR_REQUIRE(context, store.ok());
  SCR_REQUIRE(context, store.value().publish(scrtest::make_simple_revision("store.core/hall", 1, 1, "genesis", 999000),
                                             PublishOptions{})
                           .ok());
  const std::filesystem::path manifest_path = fixture.root() / "manifest";
  const std::vector<std::uint8_t> manifest = read_bytes(manifest_path);
  store.value() = Store{};

  // A manifest that declares an impossible class count is refused before any
  // allocation is attempted.
  {
    std::vector<std::uint8_t> forged = manifest;
    // payload starts after the 12 byte header; version u16 and reserved u8 and
    // the authority string come first, so rewrite the whole payload instead.
    ByteWriter writer;
    writer.put_u16(1);
    writer.put_u8(0);
    writer.put_string(kAuthority);
    writer.put_u64(1);
    writer.put_u64(2);
    writer.put_u8(0);
    writer.put_u32(0xFFFFFFFFu);
    const auto frame = encode_frame(FrameKind::StoreManifest, writer.bytes(), kMaxManifestPayloadBytes);
    SCR_REQUIRE(context, frame.ok());
    SCR_REQUIRE(context, write_bytes(manifest_path, frame.value()));
    const auto opened = Store::open(fixture.root(), StoreMode::ReadOnly);
    SCR_REQUIRE(context, !opened.ok());
    SCR_CHECK_EQ(context, opened.status().code(), ErrorCode::BoundExceeded);
  }

  // A manifest payload with a hostile history count is refused.
  {
    ByteWriter writer;
    writer.put_u16(1);
    writer.put_u8(0);
    writer.put_string(kAuthority);
    writer.put_u64(1);
    writer.put_u64(2);
    writer.put_u8(0);
    writer.put_u32(1);
    writer.put_string("store.core/hall");
    writer.put_u8(1);
    writer.put_u64(1);
    writer.put_u32(1);
    writer.put_bytes(Digest::of("x").bytes().data(), Digest::kBytes);
    writer.put_u8(0);
    writer.put_u8(0);
    writer.put_string("");
    writer.put_u32(0xFFFFFFFEu);
    const auto frame = encode_frame(FrameKind::StoreManifest, writer.bytes(), kMaxManifestPayloadBytes);
    SCR_REQUIRE(context, frame.ok());
    SCR_REQUIRE(context, write_bytes(manifest_path, frame.value()));
    const auto opened = Store::open(fixture.root(), StoreMode::ReadOnly);
    SCR_REQUIRE(context, !opened.ok());
    SCR_CHECK_EQ(context, opened.status().code(), ErrorCode::BoundExceeded);
  }

  // A manifest frame of the wrong kind is refused.
  {
    const auto manifest_frame = decode_frame(manifest.data(), manifest.size(), FrameKind::StoreManifest,
                                             kMaxManifestPayloadBytes);
    SCR_REQUIRE(context, manifest_frame.ok());
    const auto wrong = encode_frame(FrameKind::RevisionContent, manifest_frame.value().payload,
                                    kMaxManifestPayloadBytes);
    SCR_REQUIRE(context, wrong.ok());
    SCR_REQUIRE(context, write_bytes(manifest_path, wrong.value()));
    const auto opened = Store::open(fixture.root(), StoreMode::ReadOnly);
    SCR_REQUIRE(context, !opened.ok());
    SCR_CHECK_EQ(context, opened.status().code(), ErrorCode::InvalidArgument);
  }

  SCR_REQUIRE(context, write_bytes(manifest_path, manifest));

  // Path hardening: an empty path, an embedded NUL, a reserved device name, and
  // an existing regular file are all refused with a status rather than a crash.
  SCR_CHECK_EQ(context, platform::path_from_utf8("").status().code(), ErrorCode::InvalidArgument);
  SCR_CHECK_EQ(context, platform::path_from_utf8(std::string("bad\0path", 8)).status().code(),
               ErrorCode::InvalidArgument);
  SCR_CHECK_EQ(context, platform::path_from_utf8(std::string("\xC3\x28")).status().code(),
               ErrorCode::InvalidTextEncoding);

  const auto file_root = fixture.directory.path() / "not-a-directory";
  {
    std::ofstream file(file_root);
    file << "this is a file";
  }
  const auto created_on_file = Store::create(file_root, AuthorityId::parse(kAuthority).value(), Epoch::from(1),
                                             Timestamp::unset());
  SCR_CHECK_EQ(context, created_on_file.status().code(), ErrorCode::InvalidArgument);
  const auto opened_on_file = Store::open(file_root, StoreMode::ReadOnly);
  SCR_CHECK_EQ(context, opened_on_file.status().code(), ErrorCode::StoreNotInitialized);

  const auto reserved = platform::path_from_utf8((fixture.directory.path() / "CON").string());
  if (reserved.ok()) {
    const auto created_reserved =
        Store::create(reserved.value(), AuthorityId::parse(kAuthority).value(), Epoch::from(1), Timestamp::unset());
    if (created_reserved.ok()) {
      // The platform accepted the reserved name: the store must still behave,
      // and the cleanup path must be able to remove the files again.
      SCR_CHECK(context, created_reserved.value().registry().empty());
      SCR_CHECK(context, platform::remove_file(reserved.value() / "manifest").ok());
      SCR_CHECK(context, platform::remove_file(reserved.value() / "guard").ok());
    } else {
      SCR_CHECK(context, created_reserved.status().code() != ErrorCode::Internal);
    }
  }

  // A store that does not exist yet reports StoreNotInitialized.
  const auto missing = Store::open(fixture.directory.path() / "absent", StoreMode::ReadOnly);
  SCR_CHECK_EQ(context, missing.status().code(), ErrorCode::StoreNotInitialized);

  // A store root that is a junction is refused rather than followed.
  const auto junction_root = fixture.directory.path() / "junction";
  const std::string command = "cmd /c mklink /J \"" + junction_root.string() + "\" \"" +
                              (fixture.root()).string() + "\" >nul 2>&1";
  const int junction_status = std::system(command.c_str());
  if (junction_status == 0) {
    const auto junction_reparse = platform::is_reparse_point(junction_root);
    SCR_REQUIRE(context, junction_reparse.ok());
    SCR_CHECK(context, junction_reparse.value());
    const auto opened_junction = Store::open(junction_root, StoreMode::ReadOnly);
    SCR_REQUIRE(context, !opened_junction.ok());
    SCR_CHECK_EQ(context, opened_junction.status().code(), ErrorCode::InvalidArgument);
  }
}

SCR_TEST(store_handles_long_paths) {
  Fixture fixture("store-long-path");
  std::filesystem::path deep = fixture.directory.path();
  for (int index = 0; index < 12; ++index) {
    deep /= ("segment-" + std::to_string(index) + "-" + std::string(18, 'x'));
  }
  SCR_CHECK(context, deep.string().size() > 260);

  auto store = Store::create(deep, AuthorityId::parse(kAuthority).value(), Epoch::from(1),
                             Timestamp::from_unix_seconds(1700000000).value());
  if (!store.ok()) {
    context.fail("creating a store at a long path failed: " + store.status().to_string(), __FILE__, __LINE__);
    return;
  }
  SCR_REQUIRE(context, store.value().publish(scrtest::make_simple_revision("store.core/hall", 1, 1, "genesis", 999000),
                                             PublishOptions{})
                           .ok());
  store.value() = Store{};
  auto reopened = Store::open(deep, StoreMode::ReadOnly);
  SCR_REQUIRE(context, reopened.ok());
  SCR_CHECK_EQ(context, reopened.value().info().revision_count, std::size_t{1});
}

SCR_TEST(registry_restore_validates_every_field) {
  Registry registry;
  registry.set_authority(AuthorityId::parse(kAuthority).value());

  const ClassRevision content = scrtest::make_simple_revision("restore.core/hall", 1, 1, "genesis", 999000);
  const Digest digest = revision_frame_digest(content).value();

  ClassRecord record;
  record.class_id = content.class_id;
  record.generation = Generation::from(1);
  record.tip = Revision::from(1);
  record.tip_digest = digest;
  record.published_at = Timestamp::from_unix_seconds(1700000000).value();
  record.history.push_back(ClassHistoryEntry{RevisionRef{Generation::from(1), Revision::from(1), digest},
                                             Timestamp::from_unix_seconds(1700000000).value(), true});

  StoredRevision stored;
  stored.content = content;
  stored.digest = digest;
  stored.published_at = Timestamp::from_unix_seconds(1700000000).value();

  SCR_CHECK(context, registry.restore_class(record, {stored}).ok());
  SCR_CHECK(context, registry.restore_class(record, {stored}).code() == ErrorCode::DuplicateIdentity);

  Registry other;
  ClassRecord wrong_tip = record;
  wrong_tip.tip = Revision::from(2);
  SCR_CHECK_EQ(context, other.restore_class(wrong_tip, {stored}).code(), ErrorCode::CorruptStore);

  Registry third;
  StoredRevision wrong_digest = stored;
  wrong_digest.digest = Digest::of("not the content");
  SCR_CHECK_EQ(context, third.restore_class(record, {wrong_digest}).code(), ErrorCode::CorruptStore);

  Registry fourth;
  ClassRecord retired_without_reason = record;
  retired_without_reason.state = ClassState::Retired;
  SCR_CHECK_EQ(context, fourth.restore_class(retired_without_reason, {stored}).code(), ErrorCode::CorruptStore);

  Registry fifth;
  SCR_CHECK_EQ(context, fifth.restore_class(record, {}).code(), ErrorCode::CorruptStore);

  Registry sixth;
  ClassRecord empty_history = record;
  empty_history.history.clear();
  SCR_CHECK_EQ(context, sixth.restore_class(empty_history, {stored}).code(), ErrorCode::CorruptStore);
}

SCR_TEST(store_epoch_adoption_fences_the_old_epoch) {
  Fixture fixture("store-epoch");
  auto store = create_store(fixture);
  SCR_REQUIRE(context, store.ok());
  SCR_REQUIRE(context, store.value().publish(scrtest::make_simple_revision("store.core/hall", 1, 1, "genesis", 999000),
                                             PublishOptions{})
                           .ok());

  const auto committed = store.value().adopt_epoch(Epoch::from(7), Timestamp::from_unix_seconds(1700000100).value());
  SCR_REQUIRE(context, committed.ok());
  SCR_CHECK_EQ(context, store.value().info().epoch.value(), static_cast<std::uint64_t>(7));

  // A lower or equal epoch is refused, and the refusal does not change state.
  SCR_CHECK_EQ(context, store.value().adopt_epoch(Epoch::from(7), Timestamp::unset()).status().code(),
               ErrorCode::InvalidArgument);
  SCR_CHECK_EQ(context, store.value().adopt_epoch(Epoch::from(3), Timestamp::unset()).status().code(),
               ErrorCode::InvalidArgument);
  SCR_CHECK_EQ(context, store.value().info().epoch.value(), static_cast<std::uint64_t>(7));

  store.value() = Store{};
  auto reopened = Store::open(fixture.root(), StoreMode::ReadOnly);
  SCR_REQUIRE(context, reopened.ok());
  SCR_CHECK_EQ(context, reopened.value().info().epoch.value(), static_cast<std::uint64_t>(7));
  SCR_CHECK_EQ(context, reopened.value().info().sequence.value(), static_cast<std::uint64_t>(3));

  // A guard that records a newer epoch than the manifest is a rollback.
  const std::vector<std::uint8_t> manifest = read_bytes(fixture.root() / "manifest");
  const std::vector<std::uint8_t> guard = read_bytes(fixture.root() / "guard");
  const auto guard_frame = decode_frame(guard.data(), guard.size(), FrameKind::StoreGuard, kMaxGuardPayloadBytes);
  SCR_REQUIRE(context, guard_frame.ok());
  GuardRecord guard_record = decode_guard_payload(guard_frame.value().payload).value();
  guard_record.epoch = Epoch::from(99);
  const auto forged_payload = encode_guard_payload(guard_record);
  SCR_REQUIRE(context, forged_payload.ok());
  const auto forged_frame = encode_frame(FrameKind::StoreGuard, forged_payload.value(), kMaxGuardPayloadBytes);
  SCR_REQUIRE(context, forged_frame.ok());
  SCR_REQUIRE(context, write_bytes(fixture.root() / "guard", forged_frame.value()));
  const auto refused = Store::open(fixture.root(), StoreMode::ReadOnly);
  SCR_REQUIRE(context, !refused.ok());
  SCR_CHECK_EQ(context, refused.status().code(), ErrorCode::RollbackDetected);
  SCR_REQUIRE(context, write_bytes(fixture.root() / "guard", guard));
  SCR_REQUIRE(context, write_bytes(fixture.root() / "manifest", manifest));
  auto recovered = Store::open(fixture.root(), StoreMode::ReadOnly);
  SCR_REQUIRE(context, recovered.ok());
}
