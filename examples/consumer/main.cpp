// Out-of-tree consumer of the installed Service Class Registry package.
//
// It exercises the public API the way a downstream DCCP component would:
// opening or creating a store, parsing the canonical text form, publishing a
// revision, binding a decision to an exact revision, verifying the durable
// state, resolving the recorded binding again after a close and reopen, and
// comparing two revisions. Running it twice against the same store directory
// also proves restart behaviour and idempotent replay.

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "scr/registry.hpp"
#include "scr/report.hpp"
#include "scr/store.hpp"
#include "scr/text.hpp"

namespace {

const char* const kSource =
    "scr-class 1\n"
    "class consumer.core/hall-a\n"
    "generation 1\n"
    "revision 1\n"
    "lineage genesis\n"
    "obligation availability.target_ppm required 999000\n"
    "obligation availability.max_annual_downtime_seconds required 300\n"
    "obligation redundancy.topology required n+1\n"
    "obligation redundancy.minimum_independent_fault_domains required 2\n"
    "obligation redundancy.concurrent_fault_tolerance required 1\n"
    "obligation maintenance.mode required concurrent-maintainable\n"
    "obligation power.feed_count required 2\n"
    "obligation power.path_independence required dual-independent\n"
    "text title \"consumer hall\"\n";

int fail(const std::string& message) {
  std::cerr << "consumer: error: " << message << "\n";
  return 1;
}

// Opens the store, creating it when the directory has never been initialized.
scr::Result<scr::Store> open_or_create(const std::filesystem::path& root, const scr::AuthorityId& authority,
                                       const scr::Epoch& epoch, const scr::Timestamp& at) {
  auto opened = scr::Store::open(root, scr::StoreMode::ReadWrite);
  if (opened.ok()) {
    return opened;
  }
  if (opened.status().code() != scr::ErrorCode::StoreNotInitialized) {
    return opened;
  }
  return scr::Store::create(root, authority, epoch, at);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: scr_consumer <store-directory>\n";
    return 2;
  }
  const std::filesystem::path store_root(argv[1]);

  const auto parsed = scr::parse_scx(kSource);
  if (!parsed.ok()) {
    return fail("the canonical text did not parse: " + parsed.status().to_string());
  }
  const scr::ClassRevision content = parsed.value();

  const auto authority = scr::AuthorityId::parse("scr-consumer");
  const auto at = scr::Timestamp::from_unix_seconds(1700000000);
  if (!authority.ok() || !at.ok()) {
    return fail("the consumer constants are invalid");
  }

  auto store = open_or_create(store_root, authority.value(), scr::Epoch::from(1), at.value());
  if (!store.ok()) {
    return fail("opening the store failed: " + store.status().to_string());
  }

  scr::PublishOptions options;
  options.at = at.value();
  const auto published = store.value().publish(content, options);
  if (!published.ok()) {
    return fail("publication failed: " + published.status().to_string());
  }

  scr::ClassBinding binding;
  binding.class_id = content.class_id;
  binding.revision = published.value().publish.reference;

  const auto resolved = store.value().registry().resolve(binding);
  if (!resolved.ok()) {
    return fail("the binding did not resolve: " + resolved.status().to_string());
  }
  if (resolved.value().state != scr::BindingState::Fresh &&
      resolved.value().state != scr::BindingState::Superseded) {
    return fail(std::string("the binding resolved as ") + scr::to_string(resolved.value().state));
  }

  std::cout << "published " << binding.to_string()
            << (published.value().publish.idempotent ? " (idempotent replay)" : "") << "\n";
  std::cout << "binding state: " << scr::to_string(resolved.value().state) << "\n";
  std::cout << "manifest sequence " << published.value().commit.sequence.value() << ", digest "
            << published.value().commit.manifest_digest.hex() << "\n";
  std::cout << "record files written " << published.value().commit.records_written << "\n";

  // Release the writer, reopen read only, and verify every record.
  store.value() = scr::Store{};
  auto reader = scr::Store::open(store_root, scr::StoreMode::ReadOnly);
  if (!reader.ok()) {
    return fail("reopening the store read only failed: " + reader.status().to_string());
  }
  const auto report = reader.value().verify();
  if (!report.ok() || !report.value().ok) {
    return fail("store verification failed");
  }
  std::cout << "verified " << report.value().verified_records << " record(s), sequence "
            << report.value().sequence.value() << "\n";

  const auto again = reader.value().registry().resolve(binding);
  if (!again.ok()) {
    return fail("the binding did not survive the reopen: " + again.status().to_string());
  }

  const auto explanation = reader.value().registry().explain(content.class_id);
  if (!explanation.ok()) {
    return fail("explain failed: " + explanation.status().to_string());
  }
  std::cout << "authoritative revision " << explanation.value().authoritative.to_string() << "\n";
  std::cout << "effective obligations: " << explanation.value().obligations.size() << "\n";

  // Release the reader: the exclusive writer lock conflicts with any live
  // reader, which is exactly what the store lock is for.
  reader.value() = scr::Store{};

  auto writer = scr::Store::open(store_root, scr::StoreMode::ReadWrite);
  if (!writer.ok()) {
    return fail("reopening the store read-write failed: " + writer.status().to_string());
  }

  // Build the next revision from the authoritative record rather than from the
  // local object, so that the lineage is bound to what the store actually holds.
  const auto record = writer.value().registry().class_record(content.class_id);
  if (!record.ok()) {
    return fail("the class record is missing: " + record.status().to_string());
  }
  scr::ClassRevision second = content;
  second.revision = record.value().tip.next().value();
  const auto lineage = scr::Lineage::from_predecessor(
      scr::RevisionRef{record.value().generation, record.value().tip, record.value().tip_digest});
  second.lineage = lineage.value();
  scr::Metadata metadata = content.metadata;
  metadata.summary = scr::BoundedText::create("consumer hall revision two", scr::kSummaryMaxBytes).value();
  second.metadata = metadata;

  const auto second_publish = writer.value().publish(second, options);
  if (!second_publish.ok()) {
    return fail("the second publication failed: " + second_publish.status().to_string());
  }
  const auto superseded = writer.value().registry().resolve(binding);
  if (!superseded.ok() || superseded.value().state != scr::BindingState::Superseded) {
    return fail("the first binding was not reported as superseded");
  }

  const auto comparison =
      writer.value().registry().compare(published.value().publish.reference, second_publish.value().publish.reference);
  if (!comparison.ok()) {
    return fail("comparison failed: " + comparison.status().to_string());
  }
  std::cout << "first binding is now " << scr::to_string(superseded.value().state) << "\n";
  std::cout << "revision relation: " << scr::to_string(comparison.value().relation) << "\n";
  std::cout << "consumer validation complete\n";
  return 0;
}
