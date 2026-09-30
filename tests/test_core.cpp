#include "harness.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "scr/canonical.hpp"
#include "scr/composition.hpp"
#include "scr/contradiction.hpp"
#include "scr/text.hpp"

using namespace scr;
using scrtest::make_published_revision;
using scrtest::make_simple_revision;

namespace {

ObligationSet make_obligations(std::vector<std::pair<ObligationKey, std::pair<Modality, std::uint64_t>>> entries) {
  ObligationSet set;
  for (const auto& entry : entries) {
    const auto created = Obligation::create(entry.first, entry.second.first, entry.second.second);
    if (created.ok()) {
      static_cast<void>(set.add(created.value()));
    }
  }
  return set;
}

std::string hex_of(const std::string& text) {
  return Digest::of(text).hex();
}

// Builds a revision whose obligation set is exactly the supplied entries.
ClassRevision make_revision_with(
    const std::string& class_id, std::uint64_t generation, std::uint64_t revision_value,
    std::vector<std::pair<ObligationKey, std::pair<Modality, std::uint64_t>>> entries,
    std::vector<CompositionRef> composes = {}) {
  ObligationSet obligations;
  for (const auto& entry : entries) {
    const auto created = Obligation::create(entry.first, entry.second.first, entry.second.second);
    if (created.ok()) {
      static_cast<void>(obligations.add(created.value()));
    }
  }
  Metadata metadata;
  metadata.title = BoundedText::create("test service class", kTitleMaxBytes).value();
  return make_revision(ClassId::parse(class_id).value(), Generation::from(generation),
                       Revision::from(static_cast<std::uint32_t>(revision_value)), Lineage::genesis(), obligations,
                       {}, std::move(composes), metadata)
      .value();
}

const std::vector<std::pair<ObligationKey, std::pair<Modality, std::uint64_t>>>& baseline_obligations() {
  static const std::vector<std::pair<ObligationKey, std::pair<Modality, std::uint64_t>>> entries = {
      {ObligationKey::AvailabilityMaxAnnualDowntimeSeconds, {Modality::Required, 300}},
      {ObligationKey::RedundancyTopology, {Modality::Required, static_cast<std::uint64_t>(RedundancyTopology::NPlusOne)}},
      {ObligationKey::RedundancyMinimumIndependentFaultDomains, {Modality::Required, 2}},
      {ObligationKey::RedundancyConcurrentFaultTolerance, {Modality::Required, 1}},
  };
  return entries;
}

}  // namespace

SCR_TEST(digest_known_answers) {
  SCR_CHECK_EQ(context, hex_of(""),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  SCR_CHECK_EQ(context, hex_of("abc"),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  SCR_CHECK_EQ(context, hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
               std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  SCR_CHECK_EQ(context,
               hex_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrs"
                      "mnopqrstnopqrstu"),
               std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));

  // One million 'a' characters: exercises multi-block hashing and the length
  // counter.
  std::string million(1000000, 'a');
  SCR_CHECK_EQ(context, hex_of(million),
               std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

  // Incremental hashing must agree with one-shot hashing for every split.
  Sha256 incremental;
  const std::string text = "the quick brown fox jumps over the lazy dog";
  for (std::size_t index = 0; index < text.size(); ++index) {
    incremental.update(text.data() + index, 1);
  }
  const auto incremental_digest = Digest::from_bytes(incremental.finish());
  SCR_CHECK_EQ(context, incremental_digest.hex(), Digest::of(text).hex());

  SCR_CHECK_EQ(context, static_cast<std::uint64_t>(crc32("123456789")), static_cast<std::uint64_t>(0xCBF43926u));
}

SCR_TEST(digest_text_form_is_strict) {
  const Digest digest = Digest::of("abc");
  const auto parsed = Digest::parse(digest.hex());
  SCR_REQUIRE(context, parsed.ok());
  SCR_CHECK_EQ(context, parsed.value().hex(), digest.hex());

  SCR_CHECK_EQ(context, Digest::parse(digest.hex().substr(0, 63)).status().code(), ErrorCode::InvalidArgument);
  SCR_CHECK_EQ(context, Digest::parse(digest.hex() + "00").status().code(), ErrorCode::InvalidArgument);
  SCR_CHECK_EQ(context, Digest::parse("z" + digest.hex().substr(1)).status().code(), ErrorCode::InvalidArgument);
  SCR_CHECK_EQ(context, Digest::parse("").status().code(), ErrorCode::InvalidArgument);
  SCR_CHECK(context, Digest::null().is_null());
  SCR_CHECK(context, !digest.is_null());

  std::string upper = digest.hex();
  for (char& character : upper) {
    character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  }
  const auto normalized = Digest::parse(upper);
  SCR_REQUIRE(context, normalized.ok());
  SCR_CHECK_EQ(context, normalized.value().hex(), digest.hex());
}

SCR_TEST(identifiers_are_validated) {
  SCR_CHECK(context, ClassId::parse("facility.core/dc-hall-a").ok());
  SCR_CHECK(context, ClassId::parse("a/b").ok());
  SCR_CHECK(context, ClassId::compose("facility.core", "rack-1").ok());

  SCR_CHECK_EQ(context, ClassId::parse("").status().code(), ErrorCode::InvalidIdentifier);
  SCR_CHECK_EQ(context, ClassId::parse("no-separator").status().code(), ErrorCode::InvalidIdentifier);
  SCR_CHECK_EQ(context, ClassId::parse("a/b/c").status().code(), ErrorCode::InvalidIdentifier);
  SCR_CHECK_EQ(context, ClassId::parse("Facility.Core/Hall").status().code(), ErrorCode::InvalidIdentifier);
  SCR_CHECK_EQ(context, ClassId::parse("../escape").status().code(), ErrorCode::InvalidIdentifier);
  SCR_CHECK_EQ(context, ClassId::parse("a/b\\c").status().code(), ErrorCode::InvalidIdentifier);
  SCR_CHECK_EQ(context, ClassId::parse("a/..").status().code(), ErrorCode::InvalidIdentifier);
  SCR_CHECK_EQ(context, ClassId::parse("a/.hidden").status().code(), ErrorCode::InvalidIdentifier);
  SCR_CHECK_EQ(context, ClassId::parse("a/b.").status().code(), ErrorCode::InvalidIdentifier);
  SCR_CHECK_EQ(context, ClassId::parse(std::string(200, 'a')).status().code(), ErrorCode::InvalidIdentifier);
  SCR_CHECK_EQ(context, ClassId::parse(std::string("a/") + std::string(64, 'b')).status().code(),
               ErrorCode::InvalidIdentifier);

  SCR_CHECK(context, AuthorityId::parse("scr-primary").ok());
  SCR_CHECK(context, AuthorityId::parse("SCR.primary_1").ok());
  SCR_CHECK_EQ(context, AuthorityId::parse("").status().code(), ErrorCode::InvalidIdentifier);
  SCR_CHECK_EQ(context, AuthorityId::parse("-leading").status().code(), ErrorCode::InvalidIdentifier);
  SCR_CHECK_EQ(context, AuthorityId::parse("has space").status().code(), ErrorCode::InvalidIdentifier);

  SCR_CHECK(context, is_valid_identifier_path("facility.policy/tenant-floor", kReferenceTargetMaxSegments));
  SCR_CHECK(context, !is_valid_identifier_path("", kReferenceTargetMaxSegments));
  SCR_CHECK(context, !is_valid_identifier_path("/leading", kReferenceTargetMaxSegments));
  SCR_CHECK(context, !is_valid_identifier_path("trailing/", kReferenceTargetMaxSegments));
  SCR_CHECK(context, !is_valid_identifier_path("a/b/c/d/e/f/g/h/i", kReferenceTargetMaxSegments));
  SCR_CHECK(context, !is_valid_identifier_path("Upper/case", kReferenceTargetMaxSegments));
}

SCR_TEST(utf8_validation_is_strict) {
  SCR_CHECK(context, is_valid_utf8("plain ascii"));
  SCR_CHECK(context, is_valid_utf8("caf\xC3\xA9"));
  SCR_CHECK(context, is_valid_utf8("\xE2\x82\xAC"));
  SCR_CHECK(context, is_valid_utf8("\xF0\x9F\x94\x92"));

  SCR_CHECK(context, !is_valid_utf8(std::string("nul\0inside", 10)));
  SCR_CHECK(context, !is_valid_utf8("\xC0\xAF"));            // overlong solidus
  SCR_CHECK(context, !is_valid_utf8("\xE0\x80\xAF"));        // overlong
  SCR_CHECK(context, !is_valid_utf8("\xF0\x80\x80\xAF"));    // overlong
  SCR_CHECK(context, !is_valid_utf8("\xED\xA0\x80"));        // surrogate
  SCR_CHECK(context, !is_valid_utf8("\xF4\x90\x80\x80"));    // above U+10FFFF
  SCR_CHECK(context, !is_valid_utf8("\xC3"));                // truncated
  SCR_CHECK(context, !is_valid_utf8("\xC3\x28"));            // bad continuation
  SCR_CHECK(context, !is_valid_utf8("\xFF"));                // invalid lead

  SCR_CHECK(context, has_control_characters("a\nb"));
  SCR_CHECK(context, !has_control_characters("caf\xC3\xA9"));

  const auto ok = BoundedText::create("title", 32);
  SCR_CHECK(context, ok.ok());
  SCR_CHECK_EQ(context, BoundedText::create("", 32).status().code(), ErrorCode::InvalidArgument);
  SCR_CHECK_EQ(context, BoundedText::create("line\nbreak", 32).status().code(), ErrorCode::InvalidTextEncoding);
  SCR_CHECK_EQ(context, BoundedText::create(std::string(33, 'x'), 32).status().code(), ErrorCode::BoundExceeded);
}

SCR_TEST(counters_and_timestamps_never_wrap) {
  const auto overflow_generation = Generation::from(UINT64_MAX).next();
  SCR_CHECK_EQ(context, overflow_generation.status().code(), ErrorCode::Overflow);

  const auto overflow_revision = Revision::from(UINT32_MAX).next();
  SCR_CHECK_EQ(context, overflow_revision.status().code(), ErrorCode::Overflow);

  const auto overflow_sequence = Sequence::from(UINT64_MAX).next();
  SCR_CHECK_EQ(context, overflow_sequence.status().code(), ErrorCode::Overflow);

  SCR_CHECK_EQ(context, Generation::from(41).next().value().value(), static_cast<std::uint64_t>(42));
  SCR_CHECK_EQ(context, Revision::from(7).next().value().value(), static_cast<std::uint32_t>(8));

  SCR_CHECK_EQ(context, checked_add(UINT64_MAX, 1).status().code(), ErrorCode::Overflow);
  SCR_CHECK_EQ(context, checked_mul(UINT64_MAX, 2).status().code(), ErrorCode::Overflow);
  SCR_CHECK_EQ(context, checked_add(1, 2).value(), static_cast<std::uint64_t>(3));

  const Timestamp unset;
  SCR_CHECK(context, !unset.has_value());
  SCR_CHECK_EQ(context, unset.to_iso8601(), std::string("unset"));

  const auto epoch = Timestamp::from_unix_seconds(0);
  SCR_REQUIRE(context, epoch.ok());
  SCR_CHECK_EQ(context, epoch.value().to_iso8601(), std::string("1970-01-01T00:00:00Z"));

  const auto known = Timestamp::from_unix_seconds(1700000000);
  SCR_REQUIRE(context, known.ok());
  SCR_CHECK_EQ(context, known.value().to_iso8601(), std::string("2023-11-14T22:13:20Z"));

  const auto end = Timestamp::from_unix_seconds(Timestamp::kMaxUnixSeconds);
  SCR_REQUIRE(context, end.ok());
  SCR_CHECK_EQ(context, end.value().to_iso8601(), std::string("9999-12-31T23:59:59Z"));

  SCR_CHECK_EQ(context, Timestamp::from_unix_seconds(-1).status().code(), ErrorCode::OutOfRange);
  SCR_CHECK_EQ(context, Timestamp::from_unix_seconds(Timestamp::kMaxUnixSeconds + 1).status().code(),
               ErrorCode::OutOfRange);
}

SCR_TEST(obligations_are_typed_and_bounded) {
  SCR_CHECK(context, Obligation::create(ObligationKey::AvailabilityTargetPpm, Modality::Required, 999900).ok());
  SCR_CHECK_EQ(context,
               Obligation::create(ObligationKey::AvailabilityTargetPpm, Modality::Required, 1000001).status().code(),
               ErrorCode::OutOfRange);
  SCR_CHECK_EQ(context,
               Obligation::create(ObligationKey::RedundancyTopology, Modality::Required, 99).status().code(),
               ErrorCode::InvalidEnum);
  SCR_CHECK_EQ(context,
               Obligation::create(static_cast<ObligationKey>(999), Modality::Required, 1).status().code(),
               ErrorCode::InvalidEnum);
  SCR_CHECK_EQ(context,
               Obligation::create(ObligationKey::AvailabilityTargetPpm, Modality::Unspecified, 1).status().code(),
               ErrorCode::InvalidArgument);
  SCR_CHECK(context, Obligation::create(ObligationKey::AvailabilityTargetPpm, Modality::Unspecified, 0).ok());
  SCR_CHECK_EQ(context,
               Obligation::create(ObligationKey::RedundancyTopology, Modality::Required,
                                  static_cast<std::uint64_t>(RedundancyTopology::Unspecified))
                   .status()
                   .code(),
               ErrorCode::InvalidArgument);
  SCR_CHECK_EQ(context,
               Obligation::create(ObligationKey::OperationsMonitoringIntervalSeconds, Modality::Required, 0)
                   .status()
                   .code(),
               ErrorCode::OutOfRange);

  ObligationSet set;
  const auto first = Obligation::create(ObligationKey::RedundancyTopology, Modality::Required,
                                        static_cast<std::uint64_t>(RedundancyTopology::NPlusOne));
  SCR_REQUIRE(context, first.ok());
  SCR_CHECK(context, set.add(first.value()).ok());
  SCR_CHECK_EQ(context, set.add(first.value()).status().code(), ErrorCode::DuplicateObligation);
  SCR_CHECK_EQ(context, set.size(), std::size_t{1});
  SCR_CHECK(context, set.contains(ObligationKey::RedundancyTopology));
  SCR_CHECK_EQ(context, set.get(ObligationKey::PowerFeedCount).status().code(), ErrorCode::UnknownField);
  SCR_CHECK_EQ(context, describe(ObligationKey::RedundancyTopology).enum_count, std::size_t{8});
  SCR_CHECK_EQ(context, std::string(describe(ObligationKey::RedundancyTopology).name),
               std::string("redundancy.topology"));
  SCR_CHECK_EQ(context, describe(ObligationKey::RedundancyTopology).strictness, Strictness::Incomparable);
  SCR_CHECK_EQ(context, describe(ObligationKey::AvailabilityTargetPpm).strictness, Strictness::HigherIsStricter);
  SCR_CHECK_EQ(context, describe(ObligationKey::RecoveryTimeObjectiveSeconds).strictness, Strictness::LowerIsStricter);
  SCR_CHECK(context, find_key("redundancy.topology") != nullptr);
  SCR_CHECK(context, find_key("redundancy.nonexistent") == nullptr);
  SCR_CHECK_EQ(context, key_at(0).key, ObligationKey::AvailabilityTargetPpm);
  SCR_CHECK_EQ(context, describe(key_at(kObligationKeyCount - 1).key).key,
               ObligationKey::OperationsIncidentNotificationSeconds);
}

SCR_TEST(canonicalization_is_insertion_order_independent) {
  const auto id = ClassId::parse("facility.core/hall-a").value();
  const auto parent_id = ClassId::parse("facility.core/power").value();
  const auto parent_digest = Digest::of("parent");
  const auto reference_alpha = ExternalReference::create(ReferenceKind::PolicyPredicate, "policy.core/alpha",
                                                         Digest::of("alpha"), AuthorityId::parse("scr-a").value(),
                                                         BoundedText::create("note a", 64).value());
  const auto reference_beta =
      ExternalReference::create(ReferenceKind::RequirementSet, "policy.core/beta", Digest::null(), AuthorityId{},
                                BoundedText{});
  SCR_REQUIRE(context, reference_alpha.ok());
  SCR_REQUIRE(context, reference_beta.ok());
  const auto composition = CompositionRef::create(
      parent_id, RevisionRef{Generation::from(1), Revision::from(2), parent_digest});
  SCR_REQUIRE(context, composition.ok());

  Metadata metadata;
  metadata.title = BoundedText::create("Hall A", kTitleMaxBytes).value();
  metadata.summary = BoundedText::create("primary hall", kSummaryMaxBytes).value();

  const auto build = [&](bool reverse) {
    ObligationSet set;
    std::vector<ObligationKey> keys = {ObligationKey::PowerFeedCount,
                                       ObligationKey::RedundancyTopology,
                                       ObligationKey::AvailabilityTargetPpm,
                                       ObligationKey::OperationsChangeControl};
    if (reverse) {
      std::reverse(keys.begin(), keys.end());
    }
    for (const ObligationKey key : keys) {
      switch (key) {
        case ObligationKey::PowerFeedCount:
          static_cast<void>(set.add(Obligation::create(key, Modality::Required, 2).value()));
          break;
        case ObligationKey::RedundancyTopology:
          static_cast<void>(set.add(Obligation::create(
              key, Modality::Required, static_cast<std::uint64_t>(RedundancyTopology::TwoN)).value()));
          break;
        case ObligationKey::AvailabilityTargetPpm:
          static_cast<void>(set.add(Obligation::create(key, Modality::Preferred, 999900).value()));
          break;
        case ObligationKey::OperationsChangeControl:
          static_cast<void>(set.add(Obligation::create(
              key, Modality::PermittedDegraded, static_cast<std::uint64_t>(ChangeControl::Standard)).value()));
          break;
        default:
          break;
      }
    }
    std::vector<ExternalReference> references = {reference_alpha.value(), reference_beta.value()};
    if (reverse) {
      std::reverse(references.begin(), references.end());
    }
    return make_revision(id, Generation::from(1), Revision::from(3),
                         Lineage::from_predecessor(
                             RevisionRef{Generation::from(1), Revision::from(2), Digest::of("previous")})
                             .value(),
                         set, references, {composition.value()}, metadata);
  };

  const auto forward = build(false);
  const auto reversed = build(true);
  SCR_REQUIRE(context, forward.ok());
  SCR_REQUIRE(context, reversed.ok());

  const auto forward_frame = encode_revision_frame(forward.value());
  const auto reversed_frame = encode_revision_frame(reversed.value());
  SCR_REQUIRE(context, forward_frame.ok());
  SCR_REQUIRE(context, reversed_frame.ok());
  SCR_CHECK(context, forward_frame.value() == reversed_frame.value());
  SCR_CHECK_EQ(context, revision_frame_digest(forward.value()).value().hex(),
               revision_frame_digest(reversed.value()).value().hex());

  const auto decoded = decode_revision_frame(forward_frame.value().data(), forward_frame.value().size());
  SCR_REQUIRE(context, decoded.ok());
  SCR_CHECK_EQ(context, decoded.value().obligations.size(), forward.value().obligations.size());
  SCR_CHECK_EQ(context, decoded.value().metadata.summary.value(), std::string("primary hall"));
  SCR_CHECK_EQ(context, decoded.value().references.size(), std::size_t{2});
  SCR_CHECK_EQ(context, decoded.value().composes.size(), std::size_t{1});
  SCR_CHECK_EQ(context, decoded.value().lineage.predecessor().digest.hex(), Digest::of("previous").hex());
  const auto reencoded = encode_revision_frame(decoded.value());
  SCR_REQUIRE(context, reencoded.ok());
  SCR_CHECK(context, reencoded.value() == forward_frame.value());
}

SCR_TEST(canonical_frames_reject_every_mutation) {
  const ClassRevision revision = make_simple_revision("facility.core/hall-a", 1, 1, "genesis", 999000);
  const auto frame = encode_revision_frame(revision);
  SCR_REQUIRE(context, frame.ok());
  const std::vector<std::uint8_t>& bytes = frame.value();
  SCR_CHECK_EQ(context, decode_revision_frame(bytes.data(), bytes.size()).ok(), true);

  // Every single-bit change must be refused: the CRC covers the whole frame.
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<std::uint8_t> mutated = bytes;
      mutated[index] = static_cast<std::uint8_t>(mutated[index] ^ (1u << bit));
      const auto decoded = decode_revision_frame(mutated.data(), mutated.size());
      if (decoded.ok()) {
        context.fail("a single-bit mutation was accepted at byte " + std::to_string(index) + " bit " +
                         std::to_string(bit),
                     __FILE__, __LINE__);
        return;
      }
    }
  }

  // Every truncation must be refused.
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    const auto decoded = decode_revision_frame(bytes.data(), length);
    if (decoded.ok()) {
      context.fail("a truncated frame was accepted at length " + std::to_string(length), __FILE__, __LINE__);
      return;
    }
  }

  // Trailing bytes must be refused.
  std::vector<std::uint8_t> extended = bytes;
  extended.push_back(0);
  SCR_CHECK_EQ(context, decode_revision_frame(extended.data(), extended.size()).status().code(),
               ErrorCode::TruncatedInput);

  // A declared payload length beyond the bound is refused before allocation.
  std::vector<std::uint8_t> absurd = bytes;
  absurd[8] = 0xFF;
  absurd[9] = 0xFF;
  absurd[10] = 0xFF;
  absurd[11] = 0xFF;
  SCR_CHECK_EQ(context, decode_revision_frame(absurd.data(), absurd.size()).status().code(), ErrorCode::BoundExceeded);

  // The wrong frame kind is refused.
  std::vector<std::uint8_t> wrong_kind = bytes;
  wrong_kind[6] = static_cast<std::uint8_t>(FrameKind::StoreManifest);
  SCR_CHECK_EQ(context, decode_revision_frame(wrong_kind.data(), wrong_kind.size()).status().code(),
               ErrorCode::InvalidArgument);

  // An unsupported format version is refused.
  std::vector<std::uint8_t> wrong_version = bytes;
  wrong_version[4] = 9;
  SCR_CHECK_EQ(context, decode_revision_frame(wrong_version.data(), wrong_version.size()).status().code(),
               ErrorCode::UnsupportedVersion);

  // Non-zero reserved flags are refused.
  std::vector<std::uint8_t> wrong_flags = bytes;
  wrong_flags[7] = 1;
  SCR_CHECK_EQ(context, decode_revision_frame(wrong_flags.data(), wrong_flags.size()).status().code(),
               ErrorCode::CorruptStore);

  SCR_CHECK_EQ(context, decode_revision_frame(nullptr, 0).status().code(), ErrorCode::InvalidArgument);
}

SCR_TEST(rule_engine_refuses_and_advises) {
  // R-01: online maintenance with zero concurrent fault tolerance.
  ContradictionReport report = evaluate_rules(make_obligations({
      {ObligationKey::MaintenanceMode,
       {Modality::Required, static_cast<std::uint64_t>(MaintenanceMode::Online)}},
      {ObligationKey::RedundancyConcurrentFaultTolerance, {Modality::Required, 0}},
  }));
  SCR_CHECK(context, report.refused());
  SCR_REQUIRE(context, !report.violations.empty());
  SCR_CHECK_EQ(context, std::string(report.violations.front().rule), std::string("R-01"));
  SCR_CHECK_EQ(context, report.violations.front().severity, Severity::Refusal);

  // The same content as a preference is an advisory, because the preference is
  // not a hard requirement.
  report = evaluate_rules(make_obligations({
      {ObligationKey::MaintenanceMode,
       {Modality::Preferred, static_cast<std::uint64_t>(MaintenanceMode::Online)}},
      {ObligationKey::RedundancyConcurrentFaultTolerance, {Modality::Required, 0}},
  }));
  SCR_CHECK(context, !report.refused());
  SCR_CHECK_EQ(context, report.advisory_count(), std::size_t{1});

  // t-01: explicit-unspecified obligations never drive a rule.
  report = evaluate_rules(make_obligations({
      {ObligationKey::MaintenanceMode, {Modality::Unspecified, 0}},
      {ObligationKey::RedundancyConcurrentFaultTolerance, {Modality::Required, 0}},
  }));
  SCR_CHECK_EQ(context, report.violations.size(), std::size_t{0});

  // R-21 / R-05: failover without a bound is a completeness refusal.
  report = evaluate_rules(make_obligations({
      {ObligationKey::RedundancyFailoverMode, {Modality::Required, static_cast<std::uint64_t>(FailoverMode::Automatic)}},
  }));
  SCR_REQUIRE(context, !report.violations.empty());
  SCR_CHECK_EQ(context, std::string(report.violations.front().rule), std::string("R-05"));
  SCR_CHECK_EQ(context, report.violations.front().code, ErrorCode::MissingRequirement);

  // R-07 and R-08 both apply, and the primary refusal is the first rule in the
  // fixed evaluation order.
  report = evaluate_rules(make_obligations({
      {ObligationKey::AvailabilityTargetPpm, {Modality::Required, 999999}},
      {ObligationKey::AvailabilityMaxAnnualDowntimeSeconds, {Modality::Required, 5000}},
      {ObligationKey::RecoveryTimeObjectiveSeconds, {Modality::Required, 6000}},
  }));
  SCR_CHECK(context, report.refused());
  SCR_CHECK_EQ(context, report.refusal_count(), std::size_t{2});
  SCR_CHECK_EQ(context, std::string(report.primary_refusal().value().rule), std::string("R-07"));
  SCR_CHECK_EQ(context, std::string(report.violations[1].rule), std::string("R-08"));

  // R-16: geographic diversity without isolation domains.
  report = evaluate_rules(make_obligations({
      {ObligationKey::PlacementGeographicDiversity,
       {Modality::Required, static_cast<std::uint64_t>(GeographicDiversity::MultiRegion)}},
  }));
  SCR_REQUIRE(context, !report.violations.empty());
  SCR_CHECK_EQ(context, std::string(report.violations.front().rule), std::string("R-16"));

  // A consistent set produces no violations at all.
  report = evaluate_rules(make_obligations({
      {ObligationKey::AvailabilityTargetPpm, {Modality::Required, 999000}},
      {ObligationKey::AvailabilityMaxAnnualDowntimeSeconds, {Modality::Required, 300}},
      {ObligationKey::RedundancyTopology, {Modality::Required, static_cast<std::uint64_t>(RedundancyTopology::NPlusOne)}},
      {ObligationKey::RedundancyMinimumIndependentFaultDomains, {Modality::Required, 2}},
      {ObligationKey::RedundancyConcurrentFaultTolerance, {Modality::Required, 1}},
      {ObligationKey::MaintenanceMode,
       {Modality::Required, static_cast<std::uint64_t>(MaintenanceMode::ConcurrentMaintainable)}},
      {ObligationKey::PowerFeedCount, {Modality::Required, 2}},
      {ObligationKey::PowerPathIndependence,
       {Modality::Required, static_cast<std::uint64_t>(PathIndependence::DualIndependent)}},
      {ObligationKey::CoolingMode, {Modality::Required, static_cast<std::uint64_t>(CoolingMode::RedundantMechanical)}},
      {ObligationKey::CoolingIndependentPaths, {Modality::Required, 2}},
      {ObligationKey::PlacementTenantSeparation,
       {Modality::Required, static_cast<std::uint64_t>(TenantSeparation::Physical)}},
      {ObligationKey::PlacementMinimumIsolationDomains, {Modality::Required, 2}},
  }));
  for (const RuleViolation& violation : report.violations) {
    context.fail(std::string("unexpected violation ") + violation.rule + ": " + violation.detail, __FILE__, __LINE__);
  }
  SCR_CHECK_EQ(context, rule_count(), std::size_t{17});
}

SCR_TEST(composition_flattens_with_provenance) {
  const ClassRevision parent = make_simple_revision("compose.core/parent", 1, 1, "genesis", 999000);
  const auto parent_digest = revision_frame_digest(parent);
  SCR_REQUIRE(context, parent_digest.ok());

  const auto composition =
      CompositionRef::create(ClassId::parse("compose.core/parent").value(),
                             RevisionRef{Generation::from(1), Revision::from(1), parent_digest.value()});
  SCR_REQUIRE(context, composition.ok());
  std::vector<std::pair<ObligationKey, std::pair<Modality, std::uint64_t>>> stronger = baseline_obligations();
  stronger.push_back({ObligationKey::AvailabilityTargetPpm, {Modality::Required, 999900}});
  const ClassRevision child = make_revision_with("compose.core/child", 1, 1, stronger, {composition.value()});

  const auto resolver = [&](const RevisionRef& reference) -> Result<ResolvedRevision> {
    if (reference.digest == parent_digest.value()) {
      ResolvedRevision resolved;
      resolved.content = parent;
      resolved.retired = false;
      return Result<ResolvedRevision>::success(std::move(resolved));
    }
    return Result<ResolvedRevision>::failure(Status::failure(ErrorCode::UnknownRevision, "unknown parent", "test"));
  };

  const auto flattened = flatten_revision(child, resolver, FlattenOptions{});
  SCR_REQUIRE(context, flattened.ok());
  const auto effective = find_effective(flattened.value(), ObligationKey::AvailabilityTargetPpm);
  SCR_REQUIRE(context, effective.ok());
  SCR_CHECK_EQ(context, effective.value().value, static_cast<std::uint64_t>(999900));
  SCR_CHECK(context, !effective.value().inherited);
  SCR_CHECK_EQ(context, effective.value().provenance.size(), std::size_t{1});
  SCR_CHECK(context, effective.value().provenance.front().direct);
  SCR_CHECK_EQ(context, effective.value().suppressed.size(), std::size_t{1});
  SCR_CHECK_EQ(context, effective.value().suppressed.front().reason, std::string("strengthened-by-target"));
  SCR_CHECK_EQ(context, flattened.value().resolution_order.size(), std::size_t{1});

  // Weakening an inherited requirement is refused.
  std::vector<std::pair<ObligationKey, std::pair<Modality, std::uint64_t>>> weaker = baseline_obligations();
  weaker.push_back({ObligationKey::AvailabilityTargetPpm, {Modality::Required, 998000}});
  const ClassRevision weakening = make_revision_with("compose.core/child", 1, 1, weaker, {composition.value()});
  const auto refused = flatten_revision(weakening, resolver, FlattenOptions{});
  SCR_CHECK_EQ(context, refused.status().code(), ErrorCode::WeakenedRequirement);

  // Replacing an inherited requirement with "explicitly unspecified" is refused.
  std::vector<std::pair<ObligationKey, std::pair<Modality, std::uint64_t>>> unspecified_entries =
      baseline_obligations();
  unspecified_entries.push_back({ObligationKey::AvailabilityTargetPpm, {Modality::Unspecified, 0}});
  const ClassRevision unspecified =
      make_revision_with("compose.core/child", 1, 1, unspecified_entries, {composition.value()});
  const auto unspecified_result = flatten_revision(unspecified, resolver, FlattenOptions{});
  SCR_CHECK_EQ(context, unspecified_result.status().code(), ErrorCode::WeakenedRequirement);

  // A retired parent is refused for a new publication and allowed for a
  // historical explanation.
  ResolvedRevision retired_parent;
  retired_parent.content = parent;
  retired_parent.retired = true;
  const auto retired_resolver = [&](const RevisionRef&) -> Result<ResolvedRevision> {
    return Result<ResolvedRevision>::success(retired_parent);
  };
  FlattenOptions publication_options;
  publication_options.allow_retired_parents = false;
  SCR_CHECK_EQ(context, flatten_revision(child, retired_resolver, publication_options).status().code(),
               ErrorCode::RetiredClass);
  FlattenOptions historical_options;
  historical_options.allow_retired_parents = true;
  const auto historical = flatten_revision(child, retired_resolver, historical_options);
  SCR_REQUIRE(context, historical.ok());
  SCR_CHECK(context, historical.value().advisories.size() >= std::size_t{1});
}

SCR_TEST(composition_joins_parents_and_detects_cycles) {
  const ClassRevision first = make_simple_revision("join.core/first", 1, 1, "genesis", 999000);
  const ClassRevision second = make_simple_revision("join.core/second", 1, 1, "genesis", 999900);
  const auto first_digest = revision_frame_digest(first).value();
  const auto second_digest = revision_frame_digest(second).value();

  const ClassRevision child =
      make_revision_with("join.core/child", 1, 1, baseline_obligations(),
                         {CompositionRef::create(ClassId::parse("join.core/first").value(),
                                                 RevisionRef{Generation::from(1), Revision::from(1), first_digest})
                              .value(),
                          CompositionRef::create(ClassId::parse("join.core/second").value(),
                                                 RevisionRef{Generation::from(1), Revision::from(1), second_digest})
                              .value()});

  const auto resolver = [&](const RevisionRef& reference) -> Result<ResolvedRevision> {
    ResolvedRevision resolved;
    resolved.content = (reference.digest == first_digest) ? first : second;
    return Result<ResolvedRevision>::success(std::move(resolved));
  };

  const auto flattened = flatten_revision(child, resolver, FlattenOptions{});
  SCR_REQUIRE(context, flattened.ok());
  const auto effective = find_effective(flattened.value(), ObligationKey::AvailabilityTargetPpm);
  SCR_REQUIRE(context, effective.ok());
  SCR_CHECK_EQ(context, effective.value().value, static_cast<std::uint64_t>(999900));
  SCR_CHECK_EQ(context, effective.value().suppressed.size(), std::size_t{1});
  SCR_CHECK_EQ(context, effective.value().suppressed.front().reason, std::string("dominated-by-strictness"));

  // Two parents that cannot be ordered are refused rather than silently merged.
  std::vector<std::pair<ObligationKey, std::pair<Modality, std::uint64_t>>> left_entries = baseline_obligations();
  for (auto& entry : left_entries) {
    if (entry.first == ObligationKey::RedundancyTopology) {
      entry.second.second = static_cast<std::uint64_t>(RedundancyTopology::NPlusOne);
    }
  }
  std::vector<std::pair<ObligationKey, std::pair<Modality, std::uint64_t>>> right_entries = baseline_obligations();
  for (auto& entry : right_entries) {
    if (entry.first == ObligationKey::RedundancyTopology) {
      entry.second.second = static_cast<std::uint64_t>(RedundancyTopology::TwoN);
    }
  }
  const ClassRevision left = make_revision_with("join.core/left", 1, 1, left_entries);
  const ClassRevision right = make_revision_with("join.core/right", 1, 1, right_entries);
  const auto left_digest = revision_frame_digest(left).value();
  const auto right_digest = revision_frame_digest(right).value();
  const ClassRevision conflict =
      make_revision_with("join.core/conflict", 1, 1, baseline_obligations(),
                         {CompositionRef::create(ClassId::parse("join.core/left").value(),
                                                 RevisionRef{Generation::from(1), Revision::from(1), left_digest})
                              .value(),
                          CompositionRef::create(ClassId::parse("join.core/right").value(),
                                                 RevisionRef{Generation::from(1), Revision::from(1), right_digest})
                              .value()});
  const auto conflict_resolver = [&](const RevisionRef& reference) -> Result<ResolvedRevision> {
    ResolvedRevision resolved;
    resolved.content = (reference.digest == left_digest) ? left : right;
    return Result<ResolvedRevision>::success(std::move(resolved));
  };
  SCR_CHECK_EQ(context, flatten_revision(conflict, conflict_resolver, FlattenOptions{}).status().code(),
               ErrorCode::ContradictoryObligations);

  // A cycle is refused with the cycle path, even when a hostile resolver
  // produces it.
  const Digest fake_a = Digest::of("cycle-a");
  const Digest fake_b = Digest::of("cycle-b");
  const ClassRevision cycle_a =
      make_revision_with("cycle.core/a", 1, 1, baseline_obligations(),
                         {CompositionRef::create(ClassId::parse("cycle.core/b").value(),
                                                 RevisionRef{Generation::from(1), Revision::from(1), fake_b})
                              .value()});
  const ClassRevision cycle_b =
      make_revision_with("cycle.core/b", 1, 1, baseline_obligations(),
                         {CompositionRef::create(ClassId::parse("cycle.core/a").value(),
                                                 RevisionRef{Generation::from(1), Revision::from(1), fake_a})
                              .value()});
  const auto cycle_resolver = [&](const RevisionRef& reference) -> Result<ResolvedRevision> {
    ResolvedRevision resolved;
    resolved.content = (reference.digest == fake_b) ? cycle_b : cycle_a;
    return Result<ResolvedRevision>::success(std::move(resolved));
  };
  const auto cycle = flatten_revision(cycle_a, cycle_resolver, FlattenOptions{});
  SCR_CHECK_EQ(context, cycle.status().code(), ErrorCode::CycleDetected);
  SCR_CHECK(context, cycle.status().detail().find("cycle.core") != std::string::npos);

  // Self composition.
  const ClassRevision selfish =
      make_revision_with("cycle.core/self", 1, 1, baseline_obligations(),
                         {CompositionRef::create(ClassId::parse("cycle.core/self").value(),
                                                 RevisionRef{Generation::from(1), Revision::from(1), fake_a})
                              .value()});
  const auto self_resolver = [&](const RevisionRef&) -> Result<ResolvedRevision> {
    ResolvedRevision resolved;
    resolved.content = selfish;
    return Result<ResolvedRevision>::success(std::move(resolved));
  };
  SCR_CHECK_EQ(context, flatten_revision(selfish, self_resolver, FlattenOptions{}).status().code(),
               ErrorCode::CycleDetected);

  // A diamond resolves the shared ancestor exactly once.
  ClassRevision root = make_simple_revision("diamond.core/root", 1, 1, "genesis", 999000);
  const auto root_digest = revision_frame_digest(root).value();
  const ClassRevision mid_left =
      make_revision_with("diamond.core/left", 1, 1, baseline_obligations(),
                         {CompositionRef::create(ClassId::parse("diamond.core/root").value(),
                                                 RevisionRef{Generation::from(1), Revision::from(1), root_digest})
                              .value()});
  const ClassRevision mid_right =
      make_revision_with("diamond.core/right", 1, 1, baseline_obligations(),
                         {CompositionRef::create(ClassId::parse("diamond.core/root").value(),
                                                 RevisionRef{Generation::from(1), Revision::from(1), root_digest})
                              .value()});
  const auto mid_left_digest = revision_frame_digest(mid_left).value();
  const auto mid_right_digest = revision_frame_digest(mid_right).value();
  const ClassRevision diamond =
      make_revision_with("diamond.core/top", 1, 1, baseline_obligations(),
                         {CompositionRef::create(ClassId::parse("diamond.core/left").value(),
                                                 RevisionRef{Generation::from(1), Revision::from(1), mid_left_digest})
                              .value(),
                          CompositionRef::create(ClassId::parse("diamond.core/right").value(),
                                                 RevisionRef{Generation::from(1), Revision::from(1), mid_right_digest})
                              .value()});
  std::size_t resolver_calls = 0;
  const auto diamond_resolver = [&](const RevisionRef& reference) -> Result<ResolvedRevision> {
    ++resolver_calls;
    ResolvedRevision resolved;
    if (reference.digest == root_digest) {
      resolved.content = root;
    } else if (reference.digest == mid_left_digest) {
      resolved.content = mid_left;
    } else {
      resolved.content = mid_right;
    }
    return Result<ResolvedRevision>::success(std::move(resolved));
  };
  const auto diamond_result = flatten_revision(diamond, diamond_resolver, FlattenOptions{});
  SCR_REQUIRE(context, diamond_result.ok());
  SCR_CHECK_EQ(context, resolver_calls, std::size_t{3});
  SCR_CHECK_EQ(context, diamond_result.value().resolution_order.size(), std::size_t{3});

  // The depth bound is enforced.
  FlattenOptions shallow;
  shallow.max_depth = 0;
  SCR_CHECK_EQ(context, flatten_revision(diamond, diamond_resolver, shallow).status().code(), ErrorCode::BoundExceeded);
}

SCR_TEST(registry_lifecycle_and_immutability) {
  Registry registry;
  registry.set_authority(AuthorityId::parse("scr-test").value());
  registry.set_epoch(Epoch::from(1));

  const ClassRevision genesis = make_simple_revision("registry.core/hall", 1, 1, "genesis", 999000);
  const auto first = registry.publish(genesis, PublishOptions{});
  SCR_REQUIRE(context, first.ok());
  SCR_CHECK_EQ(context, first.value().reference.generation.value(), static_cast<std::uint64_t>(1));
  SCR_CHECK_EQ(context, first.value().reference.revision.value(), static_cast<std::uint32_t>(1));
  SCR_CHECK(context, !first.value().idempotent);

  // Idempotent replay resolves before staleness rejection.
  const auto replay = registry.publish(genesis, PublishOptions{});
  SCR_REQUIRE(context, replay.ok());
  SCR_CHECK(context, replay.value().idempotent);
  SCR_CHECK_EQ(context, replay.value().reference.digest.hex(), first.value().reference.digest.hex());

  // A non-genesis first revision is refused.
  Registry other;
  SCR_CHECK_EQ(context, other.publish(make_simple_revision("registry.core/new", 1, 2, "genesis", 999000), {})
                              .status()
                              .code(),
               ErrorCode::LineageMismatch);
  SCR_CHECK_EQ(context,
               other.publish(make_simple_revision("registry.core/new", 2, 1, "genesis", 999000), {}).status().code(),
               ErrorCode::LineageMismatch);

  // Appending requires the exact authoritative tip.
  const std::string ahead_lineage = "1.3@" + first.value().reference.digest.hex();
  SCR_CHECK_EQ(context,
               registry.publish(make_simple_revision("registry.core/hall", 1, 2, ahead_lineage, 999000), {})
                   .status()
                   .code(),
               ErrorCode::StaleTip);
  const std::string wrong_digest_lineage = "1.1@" + Digest::of("some other revision").hex();
  SCR_CHECK_EQ(context,
               registry.publish(make_simple_revision("registry.core/hall", 1, 2, wrong_digest_lineage, 999000), {})
                   .status()
                   .code(),
               ErrorCode::DigestMismatch);
  SCR_CHECK_EQ(context,
               registry.publish(make_simple_revision("registry.core/hall", 2, 2, "genesis", 999000), {}).status().code(),
               ErrorCode::LineageMismatch);
  SCR_CHECK_EQ(context, registry.publish(make_simple_revision("registry.core/hall", 1, 3, "genesis", 999000), {})
                              .status()
                              .code(),
               ErrorCode::LineageMismatch);
  SCR_CHECK_EQ(context, registry.publish(make_simple_revision("registry.core/hall", 1, 1, "genesis", 999001), {})
                              .status()
                              .code(),
               ErrorCode::LineageMismatch);

  // The correct append succeeds.
  const ClassRevision second = make_published_revision(registry, "registry.core/hall", 999900);
  const auto appended = registry.publish(second, PublishOptions{});
  SCR_REQUIRE(context, appended.ok());
  SCR_CHECK_EQ(context, appended.value().reference.revision.value(), static_cast<std::uint32_t>(2));
  SCR_CHECK_EQ(context, registry.revision_count(), std::size_t{2});
  SCR_CHECK(context, registry.validate_all().ok());

  // Composition against a published parent is resolved from the index.
  ClassRevision composed = make_simple_revision("registry.core/composed", 1, 1, "genesis", 999950);
  composed.composes.push_back(CompositionRef::create(ClassId::parse("registry.core/hall").value(),
                                                     appended.value().reference)
                                  .value());
  const auto composed_outcome = registry.publish(composed, PublishOptions{});
  if (!composed_outcome.ok()) {
    context.fail("publishing the composed revision failed: " + composed_outcome.status().to_string(), __FILE__,
                 __LINE__);
    return;
  }

  // A conflicting revision is refused and nothing is recorded.
  std::vector<std::pair<ObligationKey, std::pair<Modality, std::uint64_t>>> contradiction_entries =
      baseline_obligations();
  for (auto& entry : contradiction_entries) {
    if (entry.first == ObligationKey::AvailabilityMaxAnnualDowntimeSeconds) {
      entry.second.second = 300;
    }
    if (entry.first == ObligationKey::RedundancyConcurrentFaultTolerance) {
      entry.second.second = 0;
    }
  }
  contradiction_entries.push_back(
      {ObligationKey::MaintenanceMode, {Modality::Required, static_cast<std::uint64_t>(MaintenanceMode::Online)}});
  const ClassRevision contradiction =
      make_revision_with("registry.core/contradiction", 1, 1, contradiction_entries);
  const std::size_t before = registry.revision_count();
  SCR_CHECK_EQ(context, registry.publish(contradiction, PublishOptions{}).status().code(),
               ErrorCode::ContradictoryObligations);
  SCR_CHECK_EQ(context, registry.revision_count(), before);

  // Retirement refuses new revisions of the same generation and keeps history.
  const auto retired = registry.retire(ClassId::parse("registry.core/hall").value(),
                                       BoundedText::create("decommissioned", kRetireReasonMaxBytes).value(),
                                       Timestamp::from_unix_seconds(1700000000).value());
  SCR_REQUIRE(context, retired.ok());
  SCR_CHECK(context, !retired.value().already_retired);
  const auto retired_again = registry.retire(ClassId::parse("registry.core/hall").value(),
                                             BoundedText::create("decommissioned", kRetireReasonMaxBytes).value(),
                                             Timestamp::from_unix_seconds(1700000001).value());
  SCR_REQUIRE(context, retired_again.ok());
  SCR_CHECK(context, retired_again.value().already_retired);
  SCR_CHECK_EQ(context, registry.publish(make_published_revision(registry, "registry.core/hall", 999990), {})
                              .status()
                              .code(),
               ErrorCode::RetiredClass);

  // A retired class accepts an explicitly requested new generation.
  PublishOptions re_found;
  re_found.allow_new_generation = true;
  ClassRevision new_generation = make_simple_revision("registry.core/hall", 2, 1, "genesis", 999990);
  const auto re_founded = registry.publish(new_generation, re_found);
  SCR_REQUIRE(context, re_founded.ok());
  SCR_CHECK_EQ(context, re_founded.value().reference.generation.value(), static_cast<std::uint64_t>(2));
  const auto record = registry.class_record(ClassId::parse("registry.core/hall").value());
  SCR_REQUIRE(context, record.ok());
  SCR_CHECK_EQ(context, record.value().state, ClassState::Active);
  SCR_CHECK_EQ(context, record.value().history.size(), std::size_t{3});
  SCR_CHECK(context, record.value().history.front().generation_start);
  SCR_CHECK(context, record.value().history.back().generation_start);
  SCR_CHECK_EQ(context, registry.publish(make_simple_revision("registry.core/hall", 2, 1, "genesis", 999990), {})
                              .value()
                              .idempotent,
               true);
}

SCR_TEST(bindings_resolve_with_exact_diagnostics) {
  Registry registry;
  registry.set_authority(AuthorityId::parse("scr-test").value());
  registry.set_epoch(Epoch::from(1));

  const ClassRevision genesis = make_simple_revision("bindings.core/hall", 1, 1, "genesis", 999000);
  const auto first = registry.publish(genesis, {});
  SCR_REQUIRE(context, first.ok());
  const ClassRevision second = make_published_revision(registry, "bindings.core/hall", 999900);
  const auto appended = registry.publish(second, {});
  SCR_REQUIRE(context, appended.ok());

  ClassBinding binding;
  binding.class_id = ClassId::parse("bindings.core/hall").value();

  binding.revision = appended.value().reference;
  SCR_CHECK_EQ(context, registry.resolve(binding).value().state, BindingState::Fresh);

  binding.revision = first.value().reference;
  const auto superseded = registry.resolve(binding);
  SCR_CHECK_EQ(context, superseded.value().state, BindingState::Superseded);
  SCR_CHECK_EQ(context, superseded.value().authoritative_tip.revision.value(), static_cast<std::uint32_t>(2));

  binding.revision.digest = Digest::of("not the digest");
  SCR_CHECK_EQ(context, registry.resolve(binding).value().state, BindingState::DigestMismatch);

  binding.revision = appended.value().reference;
  binding.revision.digest = Digest::null();
  SCR_CHECK_EQ(context, registry.resolve(binding).value().state, BindingState::DigestMismatch);

  binding.revision.revision = Revision::from(9);
  binding.revision.digest = appended.value().reference.digest;
  SCR_CHECK_EQ(context, registry.resolve(binding).value().state, BindingState::UnknownRevision);

  binding.revision.generation = Generation::from(9);
  SCR_CHECK_EQ(context, registry.resolve(binding).value().state, BindingState::UnknownRevision);

  binding.class_id = ClassId::parse("bindings.core/missing").value();
  SCR_CHECK_EQ(context, registry.resolve(binding).value().state, BindingState::UnknownClass);

  // Retirement is reported as retired, not as stale or unknown.
  const auto retired = registry.retire(ClassId::parse("bindings.core/hall").value(),
                                       BoundedText::create("retired for the test", kRetireReasonMaxBytes).value(),
                                       Timestamp::from_unix_seconds(1700000200).value());
  SCR_REQUIRE(context, retired.ok());
  binding.class_id = ClassId::parse("bindings.core/hall").value();
  binding.revision = first.value().reference;
  SCR_CHECK_EQ(context, registry.resolve(binding).value().state, BindingState::Retired);

  // A re-founded generation fences the old generation without losing it.
  PublishOptions re_found;
  re_found.allow_new_generation = true;
  const auto re_founded = registry.publish(make_simple_revision("bindings.core/hall", 2, 1, "genesis", 999990), re_found);
  SCR_REQUIRE(context, re_founded.ok());
  binding.revision = first.value().reference;
  const auto fenced = registry.resolve(binding);
  SCR_CHECK_EQ(context, fenced.value().state, BindingState::GenerationSuperseded);
  SCR_CHECK_EQ(context, fenced.value().authoritative_tip.generation.value(), static_cast<std::uint64_t>(2));
}

SCR_TEST(comparison_and_explanation_are_deterministic) {
  Registry registry;
  registry.set_authority(AuthorityId::parse("scr-test").value());

  ClassRevision genesis = make_simple_revision("explain.core/hall", 1, 1, "genesis", 999000);
  const auto first = registry.publish(genesis, {});
  SCR_REQUIRE(context, first.ok());

  ClassRevision second = make_published_revision(registry, "explain.core/hall", 999990);
  static_cast<void>(second.obligations.add(
      Obligation::create(ObligationKey::PowerFeedCount, Modality::Required, 2).value()));
  static_cast<void>(second.obligations.add(Obligation::create(
      ObligationKey::PowerPathIndependence, Modality::Required,
      static_cast<std::uint64_t>(PathIndependence::DualIndependent))
                                               .value()));
  static_cast<void>(second.obligations.add(Obligation::create(
      ObligationKey::PowerTransferMode, Modality::Required, static_cast<std::uint64_t>(TransferMode::Automatic))
                                               .value()));
  static_cast<void>(second.obligations.add(
      Obligation::create(ObligationKey::PowerMaximumTransferSeconds, Modality::Required, 20).value()));
  second.metadata.summary = BoundedText::create("revised summary", kSummaryMaxBytes).value();
  const auto appended = registry.publish(second, {});
  SCR_REQUIRE(context, appended.ok());

  const auto report = registry.compare(first.value().reference, appended.value().reference);
  SCR_REQUIRE(context, report.ok());
  SCR_CHECK_EQ(context, report.value().relation, RevisionRelation::FromAncestorOfTo);
  SCR_CHECK(context, !report.value().identical_content);
  bool saw_availability = false;
  for (const ObligationDelta& delta : report.value().declared) {
    if (delta.key == ObligationKey::AvailabilityTargetPpm) {
      saw_availability = true;
      SCR_CHECK_EQ(context, delta.kind, DeltaKind::ValueChanged);
      SCR_CHECK_EQ(context, std::string(delta.strictness_effect), std::string("stricter"));
    }
    if (delta.key == ObligationKey::PowerFeedCount) {
      SCR_CHECK_EQ(context, delta.kind, DeltaKind::Added);
      SCR_CHECK_EQ(context, std::string(delta.strictness_effect), std::string("not-applicable"));
    }
  }
  SCR_CHECK(context, saw_availability);
  SCR_CHECK_EQ(context, report.value().metadata_changes.size(), std::size_t{1});

  const auto identical = registry.compare(first.value().reference, first.value().reference);
  SCR_REQUIRE(context, identical.ok());
  SCR_CHECK_EQ(context, identical.value().relation, RevisionRelation::Identical);
  SCR_CHECK(context, identical.value().identical_content);
  SCR_CHECK_EQ(context, identical.value().declared.size(), std::size_t{0});

  const auto reverse = registry.compare(appended.value().reference, first.value().reference);
  SCR_REQUIRE(context, reverse.ok());
  SCR_CHECK_EQ(context, reverse.value().relation, RevisionRelation::ToAncestorOfFrom);

  const auto explanation = registry.explain(ClassId::parse("explain.core/hall").value());
  SCR_REQUIRE(context, explanation.ok());
  SCR_CHECK_EQ(context, explanation.value().state, ClassState::Active);
  SCR_CHECK_EQ(context, explanation.value().authoritative.digest.hex(), appended.value().reference.digest.hex());
  SCR_CHECK_EQ(context, explanation.value().history.size(), std::size_t{2});
  for (const RuleViolation& violation : explanation.value().rules) {
    SCR_CHECK_EQ(context, violation.severity, Severity::Advisory);
  }

  // Explanation of an inherited obligation carries the declaring revision.
  ClassRevision composed = make_simple_revision("explain.core/composed", 1, 1, "genesis", 999990);
  composed.composes.push_back(CompositionRef::create(ClassId::parse("explain.core/hall").value(),
                                                     appended.value().reference)
                                  .value());
  const auto composed_outcome = registry.publish(composed, {});
  if (!composed_outcome.ok()) {
    context.fail("publishing the composed revision failed: " + composed_outcome.status().to_string(), __FILE__,
                 __LINE__);
    return;
  }
  const auto composed_explanation = registry.explain(ClassId::parse("explain.core/composed").value());
  SCR_REQUIRE(context, composed_explanation.ok());
  bool saw_inherited = false;
  for (const ObligationExplanation& obligation : composed_explanation.value().obligations) {
    if (obligation.key == ObligationKey::PowerFeedCount) {
      saw_inherited = true;
      SCR_CHECK(context, obligation.inherited);
      SCR_REQUIRE(context, !obligation.provenance.empty());
      SCR_CHECK_EQ(context, obligation.provenance.front().class_id.str(), std::string("explain.core/hall"));
    }
  }
  SCR_CHECK(context, saw_inherited);
}

SCR_TEST(text_format_round_trips_and_refuses_hostile_input) {
  std::string source;
  source += "scr-class 1\n";
  source += "class text.core/hall\n";
  source += "generation 1\n";
  source += "revision 4\n";
  source += "lineage 1.3@" + Digest::of("previous").hex() + "\n";
  source += "obligation power.feed_count required 2\n";
  source += "obligation redundancy.topology required n+1\n";
  source += "obligation availability.target_ppm preferred 999900\n";
  source += "obligation redundancy.concurrent_fault_tolerance required 1\n";
  source += "obligation redundancy.minimum_independent_fault_domains required 2\n";
  source += "text title \"Hall A and the first\"\n";
  source += "text documentation \"quotes \\\" and backslash \\\\ survive\"\n";
  source += "reference policy-predicate policy.core/alpha @" + Digest::of("alpha").hex() + " authority=scr-a\n";
  source += "reference requirement-set policy.core/beta @unbound\n";

  const auto parsed = parse_scx(source);
  if (!parsed.ok()) {
    context.fail("the canonical text source did not parse: " + parsed.status().to_string(), __FILE__, __LINE__);
    return;
  }
  const std::string emitted = emit_scx(parsed.value());
  const auto reparsed = parse_scx(emitted);
  SCR_REQUIRE(context, reparsed.ok());
  SCR_CHECK_EQ(context, revision_frame_digest(reparsed.value()).value().hex(),
               revision_frame_digest(parsed.value()).value().hex());
  SCR_CHECK_EQ(context, reparsed.value().metadata.title.value(), std::string("Hall A and the first"));
  SCR_CHECK_EQ(context, reparsed.value().metadata.documentation.value(),
               std::string("quotes \" and backslash \\ survive"));
  SCR_CHECK_EQ(context, reparsed.value().references.size(), std::size_t{2});
  bool saw_unbound = false;
  for (const ExternalReference& reference : reparsed.value().references) {
    if (reference.digest.is_null()) {
      saw_unbound = true;
    }
  }
  SCR_CHECK(context, saw_unbound);
  SCR_CHECK_EQ(context, emit_scx(reparsed.value()), emitted);

  // Comments and blank lines are ignored, and whitespace is not significant.
  const auto with_noise = parse_scx("# comment\n\n  scr-class 1\nclass text.core/hall\n generation 1\nrevision 4\n"
                                    "lineage 1.3@" + Digest::of("previous").hex() +
                                    "\nobligation   power.feed_count\trequired 2\n# trailing comment\n");
  SCR_CHECK(context, with_noise.ok());

  const std::vector<std::string> hostile = {
      "",
      "not-a-header 1\n",
      "scr-class 2\n",
      "scr-class 1\n",
      "scr-class 1\nclass text.core/hall\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\nunknown-directive x\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\nobligation unknown.key required 1\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\nobligation power.feed_count sometimes 2\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\nobligation redundancy.topology required N+1\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\nobligation power.feed_count required 999\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\nobligation power.feed_count required 2\n"
      "obligation power.feed_count required 2\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\nobligation power.feed_count required 2\n"
      "obligation power.feed_count preferred 3\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\ntext title \"unterminated\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\ntext title \"bad \\q escape\"\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\ntext unknown \"value\"\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\ntext title \"first\"\ntext title \"second\"\n",
      "scr-class 1\nclass text.core/hall\ngeneration 0\nrevision 4\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 0\n",
      "scr-class 1\nclass text.core/hall\ngeneration 99999999999999999999999999\nrevision 4\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\nlineage notaref\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\nlineage 1.3@zz\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\nparent bad @ 1.1@" +
          Digest::of("x").hex() + "\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\nreference policy-predicate x @nothex\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\nreference wrong-kind policy.core/x @unbound\n",
      "scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\nobligation power.feed_count required 2\n"
      "text title \"control \x01 character\"\n",
  };
  for (const std::string& text : hostile) {
    const auto result = parse_scx(text);
    if (result.ok()) {
      context.fail("hostile canonical text was accepted: " + text, __FILE__, __LINE__);
    }
  }

  // A missing required obligation is refused by the revision rules.
  const auto empty_class = parse_scx("scr-class 1\nclass text.core/empty\ngeneration 1\nrevision 1\nlineage genesis\n");
  SCR_CHECK_EQ(context, empty_class.status().code(), ErrorCode::MissingRequirement);

  // Invalid UTF-8 is refused before parsing.
  const std::string invalid_utf8 = std::string("scr-class 1\nclass text.core/hall\ngeneration 1\nrevision 4\n# ") +
                                   "\xC3\x28" + "\n";
  SCR_CHECK_EQ(context, parse_scx(invalid_utf8).status().code(), ErrorCode::InvalidTextEncoding);

  // An over-long line is refused.
  const std::string long_line = "scr-class 1\n" + std::string(kMaxScxLineBytes + 1, 'a') + "\n";
  SCR_CHECK_EQ(context, parse_scx(long_line).status().code(), ErrorCode::ParseError);

  // Hex frames round trip.
  const std::string frame_hex = emit_frame_hex(parsed.value());
  const auto from_hex = parse_frame_hex(frame_hex);
  SCR_REQUIRE(context, from_hex.ok());
  SCR_CHECK_EQ(context, revision_frame_digest(from_hex.value()).value().hex(),
               revision_frame_digest(parsed.value()).value().hex());
  SCR_CHECK_EQ(context, parse_frame_hex(frame_hex.substr(1)).status().code(), ErrorCode::InvalidArgument);
  SCR_CHECK_EQ(context, parse_frame_hex("zz").status().code(), ErrorCode::InvalidArgument);
}
