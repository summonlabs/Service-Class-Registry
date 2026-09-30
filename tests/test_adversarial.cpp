#include "harness.hpp"

#include <string>
#include <vector>

#include "scr/bytes.hpp"
#include "scr/canonical.hpp"
#include "scr/text.hpp"

using namespace scr;
using scrtest::make_published_revision;
using scrtest::make_simple_revision;

namespace {

std::vector<std::uint8_t> revision_payload_prefix(const std::string& class_id) {
  ByteWriter writer;
  writer.put_u16(1);
  writer.put_u8(0);
  writer.put_string(class_id);
  writer.put_u64(1);
  writer.put_u32(1);
  writer.put_u8(0);
  return writer.take();
}

Result<ClassRevision> decode_payload(const std::vector<std::uint8_t>& payload) {
  const auto frame = encode_frame(FrameKind::RevisionContent, payload, kMaxRevisionPayloadBytes);
  if (!frame.ok()) {
    return Result<ClassRevision>::failure(frame.status());
  }
  return decode_revision_frame(frame.value().data(), frame.value().size());
}

}  // namespace

SCR_TEST(adversarial_revision_payloads_are_bounded) {
  // A declared obligation count above the key universe is refused before any
  // per-entry work happens.
  {
    std::vector<std::uint8_t> payload = revision_payload_prefix("adversarial.core/a");
    ByteWriter tail;
    tail.put_u16(0xFFFF);
    payload.insert(payload.end(), tail.bytes().begin(), tail.bytes().end());
    SCR_CHECK_EQ(context, decode_payload(payload).status().code(), ErrorCode::BoundExceeded);
  }

  // A declared string length of 0xFFFFFFFF is refused rather than allocated.
  {
    std::vector<std::uint8_t> payload = revision_payload_prefix("adversarial.core/a");
    ByteWriter tail;
    tail.put_u16(1);
    tail.put_u16(static_cast<std::uint16_t>(ObligationKey::AvailabilityTargetPpm));
    tail.put_u8(static_cast<std::uint8_t>(Modality::Required));
    tail.put_u64(999000);
    tail.put_u16(0);
    tail.put_u16(0);
    tail.put_u32(0xFFFFFFFFu);
    payload.insert(payload.end(), tail.bytes().begin(), tail.bytes().end());
    SCR_CHECK_EQ(context, decode_payload(payload).status().code(), ErrorCode::BoundExceeded);
  }

  // An unsupported payload version.
  {
    std::vector<std::uint8_t> payload = revision_payload_prefix("adversarial.core/a");
    payload[0] = 2;
    payload[1] = 0;
    SCR_CHECK_EQ(context, decode_payload(payload).status().code(), ErrorCode::UnsupportedVersion);
  }

  // A non-zero reserved byte.
  {
    std::vector<std::uint8_t> payload = revision_payload_prefix("adversarial.core/a");
    payload[2] = 9;
    SCR_CHECK_EQ(context, decode_payload(payload).status().code(), ErrorCode::CorruptStore);
  }

  // An undefined lineage discriminator.
  {
    std::vector<std::uint8_t> payload = revision_payload_prefix("adversarial.core/a");
    payload[payload.size() - 1] = 7;  // the lineage byte is the last prefix byte
    SCR_CHECK_EQ(context, decode_payload(payload).status().code(), ErrorCode::CorruptStore);
  }

  // An undefined modality and an undefined obligation key.
  {
    std::vector<std::uint8_t> payload = revision_payload_prefix("adversarial.core/a");
    ByteWriter tail;
    tail.put_u16(1);
    tail.put_u16(static_cast<std::uint16_t>(ObligationKey::AvailabilityTargetPpm));
    tail.put_u8(200);
    tail.put_u64(1);
    payload.insert(payload.end(), tail.bytes().begin(), tail.bytes().end());
    SCR_CHECK_EQ(context, decode_payload(payload).status().code(), ErrorCode::InvalidEnum);
  }
  {
    std::vector<std::uint8_t> payload = revision_payload_prefix("adversarial.core/a");
    ByteWriter tail;
    tail.put_u16(1);
    tail.put_u16(60000);
    tail.put_u8(static_cast<std::uint8_t>(Modality::Required));
    tail.put_u64(1);
    payload.insert(payload.end(), tail.bytes().begin(), tail.bytes().end());
    SCR_CHECK_EQ(context, decode_payload(payload).status().code(), ErrorCode::InvalidEnum);
  }

  // Truncation inside an entry is a truncation, not a crash.
  {
    std::vector<std::uint8_t> payload = revision_payload_prefix("adversarial.core/a");
    ByteWriter tail;
    tail.put_u16(2);
    tail.put_u16(static_cast<std::uint16_t>(ObligationKey::AvailabilityTargetPpm));
    tail.put_u8(static_cast<std::uint8_t>(Modality::Required));
    payload.insert(payload.end(), tail.bytes().begin(), tail.bytes().end());
    SCR_CHECK_EQ(context, decode_payload(payload).status().code(), ErrorCode::TruncatedInput);
  }

  // Trailing bytes after a complete payload are refused.
  {
    std::vector<std::uint8_t> payload = revision_payload_prefix("adversarial.core/a");
    ByteWriter tail;
    tail.put_u16(1);
    tail.put_u16(static_cast<std::uint16_t>(ObligationKey::AvailabilityTargetPpm));
    tail.put_u8(static_cast<std::uint8_t>(Modality::Required));
    tail.put_u64(999000);
    tail.put_u16(0);
    tail.put_u16(0);
    tail.put_string("");
    tail.put_string("");
    tail.put_string("");
    tail.put_string("");
    tail.put_u8(0xAB);
    payload.insert(payload.end(), tail.bytes().begin(), tail.bytes().end());
    SCR_CHECK_EQ(context, decode_payload(payload).status().code(), ErrorCode::InvalidArgument);
  }
}

SCR_TEST(adversarial_duplicate_identities_are_refused) {
  ClassRevision revision = scrtest::make_simple_revision("adversarial.core/dupes", 1, 1, "genesis", 999000);
  const auto reference = ExternalReference::create(ReferenceKind::PolicyPredicate, "policy.core/alpha",
                                                   Digest::of("alpha"), AuthorityId{}, BoundedText{});
  SCR_REQUIRE(context, reference.ok());
  revision.references.push_back(reference.value());
  revision.references.push_back(reference.value());
  SCR_CHECK_EQ(context, canonicalize(revision).code(), ErrorCode::DuplicateIdentity);
  SCR_CHECK_EQ(context, revision.validate().code(), ErrorCode::DuplicateIdentity);

  ClassRevision compositions = scrtest::make_simple_revision("adversarial.core/compose", 1, 1, "genesis", 999000);
  const auto composition = CompositionRef::create(
      ClassId::parse("adversarial.core/parent").value(),
      RevisionRef{Generation::from(1), Revision::from(1), Digest::of("parent")});
  SCR_REQUIRE(context, composition.ok());
  compositions.composes.push_back(composition.value());
  compositions.composes.push_back(composition.value());
  SCR_CHECK_EQ(context, canonicalize(compositions).code(), ErrorCode::DuplicateIdentity);

  // Unsorted input is canonicalized rather than refused.
  ClassRevision unsorted = scrtest::make_simple_revision("adversarial.core/order", 1, 1, "genesis", 999000);
  const auto first = CompositionRef::create(
      ClassId::parse("adversarial.core/a").value(),
      RevisionRef{Generation::from(1), Revision::from(1), Digest::of("a")});
  const auto second = CompositionRef::create(
      ClassId::parse("adversarial.core/b").value(),
      RevisionRef{Generation::from(1), Revision::from(1), Digest::of("b")});
  SCR_REQUIRE(context, first.ok());
  SCR_REQUIRE(context, second.ok());
  unsorted.composes.push_back(second.value());
  unsorted.composes.push_back(first.value());
  SCR_CHECK_EQ(context, unsorted.validate().code(), ErrorCode::InvalidArgument);
  SCR_CHECK(context, canonicalize(unsorted).ok());
  SCR_CHECK(context, unsorted.validate().ok());
}

SCR_TEST(adversarial_revision_shape_is_enforced) {
  ClassRevision revision = scrtest::make_simple_revision("adversarial.core/shape", 1, 1, "genesis", 999000);

  ClassRevision zero_generation = revision;
  zero_generation.generation = Generation::from(0);
  SCR_CHECK_EQ(context, zero_generation.validate().code(), ErrorCode::InvalidArgument);

  ClassRevision zero_revision = revision;
  zero_revision.revision = Revision::from(0);
  SCR_CHECK_EQ(context, zero_revision.validate().code(), ErrorCode::InvalidArgument);

  ClassRevision null_predecessor = revision;
  const auto lineage = Lineage::from_predecessor(RevisionRef{Generation::from(1), Revision::from(0), Digest::of("x")});
  SCR_CHECK_EQ(context, lineage.status().code(), ErrorCode::InvalidArgument);
  const auto null_digest_lineage =
      Lineage::from_predecessor(RevisionRef{Generation::from(1), Revision::from(1), Digest::null()});
  SCR_CHECK_EQ(context, null_digest_lineage.status().code(), ErrorCode::InvalidArgument);

  ClassRevision self_predecessor = revision;
  self_predecessor.revision = Revision::from(4);
  const auto self_lineage = Lineage::from_predecessor(
      RevisionRef{Generation::from(1), Revision::from(4), Digest::of("self")});
  SCR_REQUIRE(context, self_lineage.ok());
  self_predecessor.lineage = self_lineage.value();
  SCR_CHECK_EQ(context, self_predecessor.validate().code(), ErrorCode::InvalidArgument);

  ClassRevision too_many_compositions = revision;
  for (std::size_t index = 0; index < kMaxCompositionsPerRevision + 1; ++index) {
    const auto composition = CompositionRef::create(
        ClassId::compose("adversarial.core", "parent-" + std::to_string(index)).value(),
        RevisionRef{Generation::from(1), Revision::from(1), Digest::of(std::to_string(index))});
    SCR_REQUIRE(context, composition.ok());
    too_many_compositions.composes.push_back(composition.value());
  }
  SCR_CHECK_EQ(context, too_many_compositions.validate().code(), ErrorCode::BoundExceeded);

  ClassRevision empty_obligations = revision;
  empty_obligations.obligations = ObligationSet{};
  SCR_CHECK_EQ(context, empty_obligations.validate().code(), ErrorCode::MissingRequirement);

  ClassRevision no_required = revision;
  no_required.obligations = ObligationSet{};
  static_cast<void>(no_required.obligations.add(
      Obligation::create(ObligationKey::AvailabilityTargetPpm, Modality::Preferred, 999000).value()));
  SCR_CHECK_EQ(context, no_required.validate().code(), ErrorCode::MissingRequirement);

  ClassRevision empty_identity = revision;
  empty_identity.class_id = ClassId{};
  SCR_CHECK_EQ(context, empty_identity.validate().code(), ErrorCode::InvalidIdentifier);

  ClassRevision bad_reference = revision;
  ExternalReference broken;
  broken.kind = ReferenceKind::PolicyPredicate;
  broken.target = "Upper/Case";
  bad_reference.references.push_back(broken);
  SCR_CHECK_EQ(context, bad_reference.validate().code(), ErrorCode::InvalidIdentifier);

  // The published digest never depends on wall-clock values: two revisions with
  // identical semantics digest identically regardless of when they are built.
  const auto first_digest = revision_frame_digest(revision);
  const auto second_digest = revision_frame_digest(revision);
  SCR_REQUIRE(context, first_digest.ok());
  SCR_REQUIRE(context, second_digest.ok());
  SCR_CHECK_EQ(context, first_digest.value().hex(), second_digest.value().hex());

  Registry registry;
  const auto overflow_generation =
      scrtest::make_simple_revision("adversarial.core/overflow", UINT64_MAX, 1, "genesis", 999000);
  const auto refused = registry.publish(overflow_generation, PublishOptions{});
  SCR_CHECK(context, !refused.ok());
}

SCR_TEST(adversarial_composition_limits_are_enforced) {
  scrtest::TempDirectory directory("adversarial-composition");
  Registry registry;
  registry.set_authority(AuthorityId::parse("scr-adversarial").value());

  // A chain longer than the depth bound is refused rather than walked forever.
  std::string previous_lineage = "genesis";
  for (int index = 0; index < 6; ++index) {
    const std::string class_id = "chain.core/level-" + std::to_string(index);
    ClassRevision revision = scrtest::make_simple_revision(class_id, 1, 1, "genesis", 999000);
    if (index > 0) {
      const std::string parent_id = "chain.core/level-" + std::to_string(index - 1);
      const auto parent = registry.class_record(ClassId::parse(parent_id).value());
      SCR_REQUIRE(context, parent.ok());
      revision = scrtest::make_simple_revision(class_id, 1, 1, "genesis", 999000);
      revision.composes.push_back(CompositionRef::create(ClassId::parse(parent_id).value(),
                                                         RevisionRef{parent.value().generation, parent.value().tip,
                                                                     parent.value().tip_digest})
                                      .value());
    }
    const auto published = registry.publish(revision, PublishOptions{});
    SCR_REQUIRE(context, published.ok());
    previous_lineage = published.value().reference.to_string();
  }
  static_cast<void>(previous_lineage);

  // Deep flattening still succeeds within the default bound.
  const auto flattened = registry.flatten_authoritative(ClassId::parse("chain.core/level-5").value());
  SCR_REQUIRE(context, flattened.ok());
  SCR_CHECK_EQ(context, flattened.value().resolution_order.size(), std::size_t{5});

  // A deliberately shallow bound refuses the same revision.
  const auto deep = registry.get(ClassId::parse("chain.core/level-5").value(), Generation::from(1), Revision::from(1));
  SCR_REQUIRE(context, deep.ok());
  FlattenOptions shallow;
  shallow.max_depth = 2;
  const auto refused = flatten_revision(
      deep.value().content, [&](const RevisionRef& reference) { return registry.get(reference).ok()
                                                                     ? Result<ResolvedRevision>::success(
                                                                           ResolvedRevision{registry.get(reference).value().content, false})
                                                                     : Result<ResolvedRevision>::failure(
                                                                           registry.get(reference).status()); },
      shallow);
  SCR_CHECK_EQ(context, refused.status().code(), ErrorCode::BoundExceeded);

  // The resolution budget is enforced too.
  FlattenOptions budget;
  budget.max_revisions = 1;
  const auto budget_refused = flatten_revision(
      deep.value().content, [&](const RevisionRef& reference) {
        const auto found = registry.get(reference);
        if (!found.ok()) {
          return Result<ResolvedRevision>::failure(found.status());
        }
        ResolvedRevision resolved;
        resolved.content = found.value().content;
        return Result<ResolvedRevision>::success(std::move(resolved));
      },
      budget);
  SCR_CHECK_EQ(context, budget_refused.status().code(), ErrorCode::BoundExceeded);
}
