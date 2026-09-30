#include "scr/registry.hpp"

#include <algorithm>
#include <set>

namespace scr {
namespace {

Diagnostic advisory(ErrorCode code, std::string subject, std::string detail) {
  Diagnostic diagnostic;
  diagnostic.code = code;
  diagnostic.severity = Severity::Advisory;
  diagnostic.subject = std::move(subject);
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

Status refuse(ErrorCode code, std::string detail, std::string subject) {
  return Status::failure(code, std::move(detail), std::move(subject));
}

// Names the effect of moving from the "from" revision to the "to" revision:
// when the origin value is the stricter one, the transition weakens the
// requirement, and the other way round.
const char* strictness_effect(StrictnessComparison::Relation relation) noexcept {
  switch (relation) {
    case StrictnessComparison::Relation::AStricter:
      return "weaker";
    case StrictnessComparison::Relation::Equal:
      return "equivalent";
    case StrictnessComparison::Relation::BStricter:
      return "stricter";
    case StrictnessComparison::Relation::Incomparable:
      return "incomparable";
  }
  return "not-applicable";
}

void append_delta(std::vector<ObligationDelta>& deltas, ObligationKey key, const Obligation* from,
                  const Obligation* to) {
  if (from == nullptr && to == nullptr) {
    return;
  }
  if (from != nullptr && to != nullptr && from->modality() == to->modality() && from->value() == to->value()) {
    return;
  }
  ObligationDelta delta;
  delta.key = key;
  delta.from_present = from != nullptr;
  delta.to_present = to != nullptr;
  if (from != nullptr) {
    delta.from_modality = from->modality();
    delta.from_value = from->value();
  }
  if (to != nullptr) {
    delta.to_modality = to->modality();
    delta.to_value = to->value();
  }
  if (from == nullptr) {
    delta.kind = DeltaKind::Added;
  } else if (to == nullptr) {
    delta.kind = DeltaKind::Removed;
  } else if (from->modality() != to->modality()) {
    delta.kind = DeltaKind::ModalityChanged;
  } else {
    delta.kind = DeltaKind::ValueChanged;
  }
  if (from != nullptr && to != nullptr) {
    delta.strictness_effect = strictness_effect(compare_strictness(key, from->value(), to->value()));
  }
  deltas.push_back(std::move(delta));
}

}  // namespace

const char* to_string(ClassState state) noexcept {
  switch (state) {
    case ClassState::Active:
      return "active";
    case ClassState::Retired:
      return "retired";
  }
  return "unknown";
}

const char* to_string(BindingState state) noexcept {
  switch (state) {
    case BindingState::Fresh:
      return "fresh";
    case BindingState::Superseded:
      return "superseded";
    case BindingState::Retired:
      return "retired";
    case BindingState::GenerationSuperseded:
      return "generation-superseded";
    case BindingState::UnknownClass:
      return "unknown-class";
    case BindingState::UnknownRevision:
      return "unknown-revision";
    case BindingState::DigestMismatch:
      return "digest-mismatch";
  }
  return "unknown";
}

const char* to_string(RevisionRelation relation) noexcept {
  switch (relation) {
    case RevisionRelation::Identical:
      return "identical";
    case RevisionRelation::FromAncestorOfTo:
      return "from-ancestor-of-to";
    case RevisionRelation::ToAncestorOfFrom:
      return "to-ancestor-of-from";
    case RevisionRelation::DifferentGeneration:
      return "different-generation";
  }
  return "unknown";
}

const Registry::Slot* Registry::find_slot(const ClassId& class_id) const {
  const auto found = slots_.find(class_id);
  if (found == slots_.end()) {
    return nullptr;
  }
  return &found->second;
}

Status Registry::index_revision(const ClassId& class_id, std::size_t position) {
  const Slot* slot = find_slot(class_id);
  if (slot == nullptr || position >= slot->revisions.size()) {
    return Status::failure(ErrorCode::Internal, "revision index is out of range", class_id.str());
  }
  const StoredRevision& stored = slot->revisions[position];
  const auto inserted = revision_index_.emplace(stored.digest, std::make_pair(class_id, position));
  if (!inserted.second) {
    return Status::failure(ErrorCode::DuplicateIdentity,
                           "a revision with digest " + stored.digest.hex() + " is already indexed", class_id.str());
  }
  return Status::success();
}

Result<StoredRevision> Registry::find_stored(const RevisionRef& reference) const {
  if (reference.digest.is_null()) {
    return Result<StoredRevision>::failure(refuse(ErrorCode::InvalidArgument,
                                                  "a revision reference must carry a content digest",
                                                  reference.to_display_string()));
  }
  const auto found = revision_index_.find(reference.digest);
  if (found == revision_index_.end()) {
    return Result<StoredRevision>::failure(refuse(
        ErrorCode::UnknownRevision, "no revision with digest " + reference.digest.hex() + " is published",
        reference.to_display_string()));
  }
  const Slot* slot = find_slot(found->second.first);
  if (slot == nullptr) {
    return Result<StoredRevision>::failure(
        Status::failure(ErrorCode::Internal, "digest index points at a missing class", reference.to_display_string()));
  }
  const StoredRevision& stored = slot->revisions[found->second.second];
  if (stored.content.generation != reference.generation || stored.content.revision != reference.revision) {
    return Result<StoredRevision>::failure(refuse(
        ErrorCode::DigestMismatch,
        "revision digest " + reference.digest.short_hex() + " belongs to " +
            std::to_string(stored.content.generation.value()) + "." + std::to_string(stored.content.revision.value()) +
            ", not to " + std::to_string(reference.generation.value()) + "." +
            std::to_string(reference.revision.value()) + " of class " + slot->record.class_id.str(),
        reference.to_display_string()));
  }
  return Result<StoredRevision>::success(stored);
}

Result<ResolvedRevision> Registry::resolve_parent(const RevisionRef& reference) const {
  const auto found = find_stored(reference);
  if (!found.ok()) {
    return Result<ResolvedRevision>::failure(found.status());
  }
  const auto slot_found = revision_index_.find(reference.digest);
  ResolvedRevision resolved;
  resolved.content = found.value().content;
  resolved.retired = false;
  if (slot_found != revision_index_.end()) {
    const Slot* slot = find_slot(slot_found->second.first);
    if (slot != nullptr) {
      resolved.retired = slot->record.state == ClassState::Retired;
    }
  }
  return Result<ResolvedRevision>::success(std::move(resolved));
}

Result<PublishOutcome> Registry::publish(const ClassRevision& content, const PublishOptions& options) {
  // P1: structural validation of a canonicalized copy.
  ClassRevision canonical = content;
  Status status = canonicalize(canonical);
  if (!status.ok()) {
    return Result<PublishOutcome>::failure(status);
  }
  status = canonical.validate();
  if (!status.ok()) {
    return Result<PublishOutcome>::failure(status);
  }

  // P2: content digest, then idempotent replay before ordinary staleness
  // rejection so that a lost response can always be retried.
  const auto computed = revision_frame_digest(canonical);
  if (!computed.ok()) {
    return Result<PublishOutcome>::failure(computed.status());
  }
  const Digest digest = computed.value();

  const auto existing_slot = slots_.find(canonical.class_id);
  if (existing_slot != slots_.end()) {
    for (const StoredRevision& stored : existing_slot->second.revisions) {
      if (stored.digest == digest) {
        PublishOutcome outcome;
        outcome.reference.generation = stored.content.generation;
        outcome.reference.revision = stored.content.revision;
        outcome.reference.digest = stored.digest;
        outcome.idempotent = true;
        outcome.advisories.push_back(advisory(
            ErrorCode::IdempotentReplay, canonical.class_id.str(),
            "the exact revision content is already published as " + outcome.reference.to_display_string() +
                "; the replay was accepted as an idempotent no-op"));
        return Result<PublishOutcome>::success(std::move(outcome));
      }
    }
  }

  // P3: composition resolution and merge.
  FlattenOptions flatten_options;
  flatten_options.allow_retired_parents = options.allow_retired_parents;
  flatten_options.max_depth = options.max_depth;
  flatten_options.max_revisions = options.max_revisions;

  const auto flattened = flatten_revision(
      canonical, [this](const RevisionRef& reference) { return this->resolve_parent(reference); }, flatten_options);
  if (!flattened.ok()) {
    return Result<PublishOutcome>::failure(flattened.status());
  }

  // P4: cross-key consistency of the effective semantics.
  const ObligationSet effective = effective_obligation_set(flattened.value());
  const ContradictionReport rules = evaluate_rules(effective);

  std::vector<Diagnostic> advisories = flattened.value().advisories;
  for (const RuleViolation& violation : rules.violations) {
    Diagnostic diagnostic;
    diagnostic.code = violation.code;
    diagnostic.severity = violation.severity;
    diagnostic.subject = violation.rule;
    diagnostic.detail = violation.detail;
    advisories.push_back(std::move(diagnostic));
  }

  if (rules.refused()) {
    const auto primary = rules.primary_refusal();
    Diagnostic head;
    head.code = primary.value().code;
    head.severity = Severity::Refusal;
    head.subject = primary.value().rule;
    head.detail = primary.value().detail;
    std::vector<Diagnostic> secondary;
    for (const RuleViolation& violation : rules.violations) {
      if (violation.rule == primary.value().rule) {
        continue;
      }
      Diagnostic diagnostic;
      diagnostic.code = violation.code;
      diagnostic.severity = violation.severity;
      diagnostic.subject = violation.rule;
      diagnostic.detail = violation.detail;
      secondary.push_back(std::move(diagnostic));
    }
    for (const Diagnostic& diagnostic : flattened.value().advisories) {
      secondary.push_back(diagnostic);
    }
    return Result<PublishOutcome>::failure(Status::failure(std::move(head), std::move(secondary)));
  }

  // P5: class state, lineage, and tip continuity.
  enum class Action { Create, Append, NewGeneration };
  Action action = Action::Create;
  Generation target_generation = canonical.generation;
  Revision target_revision = canonical.revision;

  if (existing_slot == slots_.end()) {
    if (!canonical.lineage.is_genesis()) {
      return Result<PublishOutcome>::failure(refuse(
          ErrorCode::LineageMismatch,
          "class '" + canonical.class_id.str() + "' has no published revision; the first revision must be a genesis "
          "revision at generation 1 revision 1", canonical.class_id.str()));
    }
    if (canonical.generation.value() != 1 || canonical.revision.value() != 1) {
      return Result<PublishOutcome>::failure(refuse(
          ErrorCode::LineageMismatch,
          "a new class starts at generation 1 revision 1, got " + std::to_string(canonical.generation.value()) + "." +
              std::to_string(canonical.revision.value()), canonical.class_id.str()));
    }
  } else {
    const ClassRecord& record = existing_slot->second.record;
    const bool retired = record.state == ClassState::Retired;

    if (canonical.lineage.is_genesis()) {
      if (!options.allow_new_generation) {
        return Result<PublishOutcome>::failure(refuse(
            ErrorCode::LineageMismatch,
            "class '" + canonical.class_id.str() + "' is already published at generation " +
                std::to_string(record.generation.value()) +
                "; a new generation must be requested explicitly and must name its lineage",
            canonical.class_id.str()));
      }
      const auto expected = record.generation.next();
      if (!expected.ok()) {
        return Result<PublishOutcome>::failure(expected.status());
      }
      if (canonical.generation != expected.value() || canonical.revision.value() != 1) {
        return Result<PublishOutcome>::failure(refuse(
            ErrorCode::LineageMismatch,
            "a new generation of class '" + canonical.class_id.str() + "' must be generation " +
                std::to_string(expected.value().value()) + " revision 1, got " +
                std::to_string(canonical.generation.value()) + "." + std::to_string(canonical.revision.value()),
            canonical.class_id.str()));
      }
      action = Action::NewGeneration;
    } else {
      if (retired) {
        return Result<PublishOutcome>::failure(refuse(
            ErrorCode::RetiredClass,
            "class '" + canonical.class_id.str() + "' is retired; a retired class accepts no further revision "
            "unless a new generation is requested explicitly",
            canonical.class_id.str()));
      }
      const RevisionRef& predecessor = canonical.lineage.predecessor();
      if (predecessor.generation != record.generation || canonical.generation != record.generation) {
        return Result<PublishOutcome>::failure(refuse(
            ErrorCode::LineageMismatch,
            "the revision names predecessor generation " + std::to_string(predecessor.generation.value()) +
                " but the authoritative generation of class '" + canonical.class_id.str() + "' is " +
                std::to_string(record.generation.value()),
            canonical.class_id.str()));
      }
      if (predecessor.revision != record.tip) {
        return Result<PublishOutcome>::failure(refuse(
            ErrorCode::StaleTip,
            "the revision names " + predecessor.to_display_string() + " as its predecessor, but the authoritative tip "
            "of class '" + canonical.class_id.str() + "' is generation " + std::to_string(record.generation.value()) +
                " revision " + std::to_string(record.tip.value()) + " with digest " + record.tip_digest.hex(),
            canonical.class_id.str()));
      }
      if (predecessor.digest != record.tip_digest) {
        return Result<PublishOutcome>::failure(refuse(
            ErrorCode::DigestMismatch,
            "the predecessor digest " + predecessor.digest.hex() + " does not match the authoritative tip digest " +
                record.tip_digest.hex() + " of class '" + canonical.class_id.str() + "'",
            canonical.class_id.str()));
      }
      const auto expected = record.tip.next();
      if (!expected.ok()) {
        return Result<PublishOutcome>::failure(expected.status());
      }
      if (canonical.revision != expected.value()) {
        return Result<PublishOutcome>::failure(refuse(
            ErrorCode::StaleTip,
            "revision numbers are strictly increasing and contiguous: class '" + canonical.class_id.str() +
                "' is at revision " + std::to_string(record.tip.value()) + ", got " +
                std::to_string(canonical.revision.value()),
            canonical.class_id.str()));
      }
      action = Action::Append;
      target_generation = record.generation;
      target_revision = canonical.revision;
    }
  }

  // P6: commit into the in-memory index.
  StoredRevision stored;
  stored.content = canonical;
  stored.digest = digest;
  stored.published_at = options.at;

  PublishOutcome outcome;
  outcome.reference.generation = target_generation;
  outcome.reference.revision = target_revision;
  outcome.reference.digest = digest;
  outcome.advisories = std::move(advisories);

  if (action == Action::Create) {
    Slot slot;
    slot.record.class_id = canonical.class_id;
    slot.record.generation = canonical.generation;
    slot.record.state = ClassState::Active;
    slot.record.tip = canonical.revision;
    slot.record.tip_digest = digest;
    slot.record.published_at = options.at;
    slot.record.history.push_back(ClassHistoryEntry{outcome.reference, options.at, true});
    slot.revisions.push_back(std::move(stored));
    const auto inserted = slots_.emplace(canonical.class_id, std::move(slot));
    if (!inserted.second) {
      return Result<PublishOutcome>::failure(
          Status::failure(ErrorCode::Internal, "class slot was inserted concurrently", canonical.class_id.str()));
    }
    const Status indexed = index_revision(canonical.class_id, 0);
    if (!indexed.ok()) {
      slots_.erase(canonical.class_id);
      return Result<PublishOutcome>::failure(indexed);
    }
    return Result<PublishOutcome>::success(std::move(outcome));
  }

  Slot& slot = existing_slot->second;
  slot.revisions.push_back(std::move(stored));
  const std::size_t position = slot.revisions.size() - 1;
  if (action == Action::NewGeneration) {
    slot.record.generation = canonical.generation;
    slot.record.state = ClassState::Active;
    slot.record.retired_at = Timestamp::unset();
    slot.record.retire_reason = BoundedText{};
  }
  slot.record.tip = canonical.revision;
  slot.record.tip_digest = digest;
  slot.record.published_at = options.at;
  slot.record.history.push_back(ClassHistoryEntry{outcome.reference, options.at, action == Action::NewGeneration});
  const Status indexed = index_revision(canonical.class_id, position);
  if (!indexed.ok()) {
    slot.record.history.pop_back();
    slot.revisions.pop_back();
    return Result<PublishOutcome>::failure(indexed);
  }
  return Result<PublishOutcome>::success(std::move(outcome));
}

Result<BindingResolution> Registry::resolve(const ClassBinding& binding) const {
  BindingResolution resolution;
  resolution.binding = binding;

  const Slot* slot = find_slot(binding.class_id);
  if (slot == nullptr) {
    resolution.state = BindingState::UnknownClass;
    resolution.detail = "class '" + binding.class_id.str() + "' has never been published by this authority";
    return Result<BindingResolution>::success(std::move(resolution));
  }

  resolution.tip_present = true;
  resolution.authoritative_tip.generation = slot->record.generation;
  resolution.authoritative_tip.revision = slot->record.tip;
  resolution.authoritative_tip.digest = slot->record.tip_digest;

  if (binding.revision.generation != slot->record.generation) {
    if (binding.revision.generation < slot->record.generation) {
      resolution.state = BindingState::GenerationSuperseded;
      resolution.detail = "the bound revision belongs to generation " +
                          std::to_string(binding.revision.generation.value()) +
                          ", which was fenced by the authoritative generation " +
                          std::to_string(slot->record.generation.value()) + "; the binding must be re-established";
      return Result<BindingResolution>::success(std::move(resolution));
    }
    resolution.state = BindingState::UnknownRevision;
    resolution.detail = "generation " + std::to_string(binding.revision.generation.value()) +
                        " of class '" + binding.class_id.str() + "' was never published";
    return Result<BindingResolution>::success(std::move(resolution));
  }

  const StoredRevision* found = nullptr;
  for (const StoredRevision& stored : slot->revisions) {
    if (stored.content.generation == binding.revision.generation &&
        stored.content.revision == binding.revision.revision) {
      found = &stored;
      break;
    }
  }
  if (found == nullptr) {
    resolution.state = BindingState::UnknownRevision;
    resolution.detail = "revision " + std::to_string(binding.revision.revision.value()) + " of generation " +
                        std::to_string(binding.revision.generation.value()) + " was never published";
    return Result<BindingResolution>::success(std::move(resolution));
  }

  if (binding.revision.digest.is_null() || found->digest != binding.revision.digest) {
    resolution.state = BindingState::DigestMismatch;
    resolution.detail = "the bound digest " +
                        (binding.revision.digest.is_null() ? std::string("(null)") : binding.revision.digest.hex()) +
                        " does not match the published digest " + found->digest.hex();
    return Result<BindingResolution>::success(std::move(resolution));
  }

  if (slot->record.state == ClassState::Retired) {
    resolution.state = BindingState::Retired;
    resolution.detail = "class '" + binding.class_id.str() +
                        "' is retired; the bound revision remains verifiable but no further revision of this "
                        "generation will be published";
    return Result<BindingResolution>::success(std::move(resolution));
  }

  const bool is_tip = binding.revision.generation == slot->record.generation &&
                      binding.revision.revision == slot->record.tip;
  if (is_tip) {
    resolution.state = BindingState::Fresh;
    resolution.detail = "the binding is exactly the authoritative tip";
    return Result<BindingResolution>::success(std::move(resolution));
  }

  resolution.state = BindingState::Superseded;
  resolution.detail = "a newer revision of class '" + binding.class_id.str() + "' is authoritative: " +
                      std::to_string(slot->record.generation.value()) + "." +
                      std::to_string(slot->record.tip.value()) + " with digest " + slot->record.tip_digest.hex();
  return Result<BindingResolution>::success(std::move(resolution));
}

Result<RevisionRecord> Registry::get(const RevisionRef& reference) const {
  const auto stored = find_stored(reference);
  if (!stored.ok()) {
    return Result<RevisionRecord>::failure(stored.status());
  }
  const auto index = revision_index_.find(reference.digest);
  RevisionRecord record;
  record.content = stored.value().content;
  record.digest = stored.value().digest;
  record.published_at = stored.value().published_at;
  if (index != revision_index_.end()) {
    const Slot* slot = find_slot(index->second.first);
    if (slot != nullptr) {
      record.state = slot->record.state;
    }
  }
  return Result<RevisionRecord>::success(std::move(record));
}

Result<RevisionRecord> Registry::get(const ClassId& class_id, Generation generation, Revision revision) const {
  const Slot* slot = find_slot(class_id);
  if (slot == nullptr) {
    return Result<RevisionRecord>::failure(
        refuse(ErrorCode::UnknownClass, "class '" + class_id.str() + "' has never been published", class_id.str()));
  }
  for (const StoredRevision& stored : slot->revisions) {
    if (stored.content.generation == generation && stored.content.revision == revision) {
      RevisionRecord record;
      record.content = stored.content;
      record.digest = stored.digest;
      record.published_at = stored.published_at;
      record.state = slot->record.state;
      return Result<RevisionRecord>::success(std::move(record));
    }
  }
  return Result<RevisionRecord>::failure(refuse(
      ErrorCode::UnknownRevision,
      "class '" + class_id.str() + "' has no revision " + std::to_string(generation.value()) + "." +
          std::to_string(revision.value()),
      class_id.str()));
}

Result<ClassRecord> Registry::class_record(const ClassId& class_id) const {
  const Slot* slot = find_slot(class_id);
  if (slot == nullptr) {
    return Result<ClassRecord>::failure(
        refuse(ErrorCode::UnknownClass, "class '" + class_id.str() + "' has never been published", class_id.str()));
  }
  return Result<ClassRecord>::success(slot->record);
}

std::vector<ClassRecord> Registry::class_records() const {
  std::vector<ClassRecord> records;
  records.reserve(slots_.size());
  for (const auto& entry : slots_) {
    records.push_back(entry.second.record);
  }
  return records;
}

std::size_t Registry::revision_count() const noexcept {
  std::size_t count = 0;
  for (const auto& entry : slots_) {
    count += entry.second.revisions.size();
  }
  return count;
}

Result<std::vector<StoredRevision>> Registry::stored_history(const ClassId& class_id) const {
  const Slot* slot = find_slot(class_id);
  if (slot == nullptr) {
    return Result<std::vector<StoredRevision>>::failure(
        refuse(ErrorCode::UnknownClass, "class '" + class_id.str() + "' has never been published", class_id.str()));
  }
  return Result<std::vector<StoredRevision>>::success(slot->revisions);
}

Result<FlattenedSemantics> Registry::flatten(const RevisionRef& reference) const {
  const auto stored = find_stored(reference);
  if (!stored.ok()) {
    return Result<FlattenedSemantics>::failure(stored.status());
  }
  FlattenOptions options;
  options.allow_retired_parents = true;
  return flatten_revision(stored.value().content, reference,
                          [this](const RevisionRef& parent) { return this->resolve_parent(parent); }, options);
}

Result<FlattenedSemantics> Registry::flatten_authoritative(const ClassId& class_id) const {
  const Slot* slot = find_slot(class_id);
  if (slot == nullptr) {
    return Result<FlattenedSemantics>::failure(
        refuse(ErrorCode::UnknownClass, "class '" + class_id.str() + "' has never been published", class_id.str()));
  }
  RevisionRef tip;
  tip.generation = slot->record.generation;
  tip.revision = slot->record.tip;
  tip.digest = slot->record.tip_digest;
  return flatten(tip);
}

Result<CompareReport> Registry::compare(const RevisionRef& from, const RevisionRef& to) const {
  const auto from_index = revision_index_.find(from.digest);
  const auto to_index = revision_index_.find(to.digest);
  if (from_index == revision_index_.end()) {
    return Result<CompareReport>::failure(
        refuse(ErrorCode::UnknownRevision, "revision " + from.to_display_string() + " is not published",
               from.to_display_string()));
  }
  if (to_index == revision_index_.end()) {
    return Result<CompareReport>::failure(refuse(
        ErrorCode::UnknownRevision, "revision " + to.to_display_string() + " is not published", to.to_display_string()));
  }
  if (from_index->second.first != to_index->second.first) {
    return Result<CompareReport>::failure(refuse(
        ErrorCode::InvalidArgument,
        "revisions belong to different classes ('" + from_index->second.first.str() + "' and '" +
            to_index->second.first.str() + "')",
        from_index->second.first.str()));
  }

  const Slot* slot = find_slot(from_index->second.first);
  if (slot == nullptr) {
    return Result<CompareReport>::failure(
        Status::failure(ErrorCode::Internal, "digest index points at a missing class", from.to_display_string()));
  }

  const auto from_stored = find_stored(from);
  const auto to_stored = find_stored(to);
  if (!from_stored.ok()) {
    return Result<CompareReport>::failure(from_stored.status());
  }
  if (!to_stored.ok()) {
    return Result<CompareReport>::failure(to_stored.status());
  }

  CompareReport report;
  report.class_id = slot->record.class_id;
  report.from = from;
  report.to = to;
  report.identical_content = from.digest == to.digest;

  if (report.identical_content) {
    report.relation = RevisionRelation::Identical;
  } else if (from.generation != to.generation) {
    report.relation = RevisionRelation::DifferentGeneration;
  } else if (from.revision < to.revision) {
    report.relation = RevisionRelation::FromAncestorOfTo;
  } else {
    report.relation = RevisionRelation::ToAncestorOfFrom;
  }

  const ClassRevision& from_content = from_stored.value().content;
  const ClassRevision& to_content = to_stored.value().content;

  for (std::size_t index = 0; index < kObligationKeyCount; ++index) {
    const ObligationKey key = key_at(index).key;
    const auto from_obligation = from_content.obligations.get(key);
    const auto to_obligation = to_content.obligations.get(key);
    append_delta(report.declared, key, from_obligation.ok() ? &from_obligation.value() : nullptr,
                 to_obligation.ok() ? &to_obligation.value() : nullptr);
  }

  const auto from_flattened = flatten(from);
  const auto to_flattened = flatten(to);
  if (!from_flattened.ok()) {
    return Result<CompareReport>::failure(from_flattened.status());
  }
  if (!to_flattened.ok()) {
    return Result<CompareReport>::failure(to_flattened.status());
  }
  for (std::size_t index = 0; index < kObligationKeyCount; ++index) {
    const ObligationKey key = key_at(index).key;
    const auto from_effective = find_effective(from_flattened.value(), key);
    const auto to_effective = find_effective(to_flattened.value(), key);
    Obligation from_obligation;
    Obligation to_obligation;
    const Obligation* from_pointer = nullptr;
    const Obligation* to_pointer = nullptr;
    if (from_effective.ok()) {
      const auto created = Obligation::create(key, from_effective.value().modality, from_effective.value().value);
      if (created.ok()) {
        from_obligation = created.value();
        from_pointer = &from_obligation;
      }
    }
    if (to_effective.ok()) {
      const auto created = Obligation::create(key, to_effective.value().modality, to_effective.value().value);
      if (created.ok()) {
        to_obligation = created.value();
        to_pointer = &to_obligation;
      }
    }
    append_delta(report.effective, key, from_pointer, to_pointer);
  }

  {
    std::set<std::string> from_references;
    std::set<std::string> to_references;
    for (const ExternalReference& reference : from_content.references) {
      from_references.insert(reference.to_string());
    }
    for (const ExternalReference& reference : to_content.references) {
      to_references.insert(reference.to_string());
    }
    for (const std::string& text : from_references) {
      if (to_references.find(text) == to_references.end()) {
        report.reference_changes.push_back("- " + text);
      }
    }
    for (const std::string& text : to_references) {
      if (from_references.find(text) == from_references.end()) {
        report.reference_changes.push_back("+ " + text);
      }
    }
  }

  {
    std::set<std::string> from_compositions;
    std::set<std::string> to_compositions;
    for (const CompositionRef& composition : from_content.composes) {
      from_compositions.insert(composition.to_string());
    }
    for (const CompositionRef& composition : to_content.composes) {
      to_compositions.insert(composition.to_string());
    }
    for (const std::string& text : from_compositions) {
      if (to_compositions.find(text) == to_compositions.end()) {
        report.composition_changes.push_back("- " + text);
      }
    }
    for (const std::string& text : to_compositions) {
      if (from_compositions.find(text) == from_compositions.end()) {
        report.composition_changes.push_back("+ " + text);
      }
    }
  }

  const auto metadata_change = [&report](const char* field, const std::string& before, const std::string& after) {
    if (before != after) {
      report.metadata_changes.push_back(std::string(field) + ": '" + before + "' -> '" + after + "'");
    }
  };
  metadata_change("title", from_content.metadata.title.value(), to_content.metadata.title.value());
  metadata_change("summary", from_content.metadata.summary.value(), to_content.metadata.summary.value());
  metadata_change("owner", from_content.metadata.owner.value(), to_content.metadata.owner.value());
  metadata_change("documentation", from_content.metadata.documentation.value(),
                  to_content.metadata.documentation.value());

  return Result<CompareReport>::success(std::move(report));
}

Result<ClassExplanation> Registry::explain(const ClassId& class_id) const {
  const Slot* slot = find_slot(class_id);
  if (slot == nullptr) {
    return Result<ClassExplanation>::failure(
        refuse(ErrorCode::UnknownClass, "class '" + class_id.str() + "' has never been published", class_id.str()));
  }
  RevisionRef tip;
  tip.generation = slot->record.generation;
  tip.revision = slot->record.tip;
  tip.digest = slot->record.tip_digest;
  return explain(tip);
}

Result<ClassExplanation> Registry::explain(const RevisionRef& reference) const {
  const auto stored = find_stored(reference);
  if (!stored.ok()) {
    return Result<ClassExplanation>::failure(stored.status());
  }
  const Slot* slot = find_slot(stored.value().content.class_id);
  if (slot == nullptr) {
    return Result<ClassExplanation>::failure(
        Status::failure(ErrorCode::Internal, "digest index points at a missing class", reference.to_display_string()));
  }
  const auto flattened = flatten(reference);
  if (!flattened.ok()) {
    return Result<ClassExplanation>::failure(flattened.status());
  }

  ClassExplanation explanation;
  explanation.class_id = slot->record.class_id;
  explanation.state = slot->record.state;
  explanation.tip_present = true;
  explanation.authoritative = reference;
  explanation.published_at = stored.value().published_at;
  explanation.retired_at = slot->record.retired_at;
  explanation.retire_reason = slot->record.retire_reason.value();
  explanation.references = stored.value().content.references;
  explanation.composes = stored.value().content.composes;
  explanation.advisories = flattened.value().advisories;
  explanation.resolution_order = flattened.value().resolution_order;
  explanation.history = slot->record.history;

  for (const EffectiveObligation& effective : flattened.value().obligations) {
    ObligationExplanation item;
    item.key = effective.key;
    item.modality = effective.modality;
    item.value = effective.value;
    item.inherited = effective.inherited;
    item.provenance = effective.provenance;
    item.suppressed = effective.suppressed;
    explanation.obligations.push_back(std::move(item));
  }

  const ContradictionReport rules = evaluate_rules(effective_obligation_set(flattened.value()));
  explanation.rules = rules.violations;
  return Result<ClassExplanation>::success(std::move(explanation));
}

Result<RetireOutcome> Registry::retire(const ClassId& class_id, const BoundedText& reason, const Timestamp& at) {
  const auto found = slots_.find(class_id);
  if (found == slots_.end()) {
    return Result<RetireOutcome>::failure(
        refuse(ErrorCode::UnknownClass, "class '" + class_id.str() + "' has never been published", class_id.str()));
  }
  if (reason.empty()) {
    return Result<RetireOutcome>::failure(refuse(ErrorCode::InvalidArgument,
                                                 "retirement requires a non-empty reason", class_id.str()));
  }
  if (!at.has_value()) {
    return Result<RetireOutcome>::failure(refuse(
        ErrorCode::InvalidArgument, "retirement requires a recorded time; an unset timestamp is refused",
        class_id.str()));
  }
  Slot& slot = found->second;
  RetireOutcome outcome;
  outcome.class_id = class_id;
  outcome.tip.generation = slot.record.generation;
  outcome.tip.revision = slot.record.tip;
  outcome.tip.digest = slot.record.tip_digest;
  outcome.retired_at = slot.record.retired_at;
  if (slot.record.state == ClassState::Retired) {
    outcome.already_retired = true;
    return Result<RetireOutcome>::success(std::move(outcome));
  }
  slot.record.state = ClassState::Retired;
  slot.record.retired_at = at;
  slot.record.retire_reason = reason;
  outcome.retired_at = at;
  outcome.already_retired = false;
  return Result<RetireOutcome>::success(std::move(outcome));
}

Status Registry::verify_class_payload(const ClassRecord& record,
                                       const std::vector<StoredRevision>& revisions) const {
  if (record.class_id.empty()) {
    return refuse(ErrorCode::InvalidIdentifier, "restored class identity is empty", "restore");
  }
  const auto parsed = ClassId::parse(record.class_id.str());
  if (!parsed.ok()) {
    return parsed.status();
  }
  if (record.generation.is_zero() || record.tip.is_zero()) {
    return refuse(ErrorCode::CorruptStore, "restored class record has a zero generation or tip", record.class_id.str());
  }
  if (record.tip_digest.is_null()) {
    return refuse(ErrorCode::CorruptStore, "restored class record has a null tip digest", record.class_id.str());
  }
  if (record.state == ClassState::Retired) {
    if (record.retire_reason.empty() || !record.retired_at.has_value()) {
      return refuse(ErrorCode::CorruptStore, "a retired class record must carry a reason and a retirement time",
                    record.class_id.str());
    }
  } else if (record.state != ClassState::Active) {
    return refuse(ErrorCode::CorruptStore, "restored class state is not a defined lifecycle state",
                  record.class_id.str());
  }
  if (revisions.empty()) {
    return refuse(ErrorCode::CorruptStore, "restored class has no revisions", record.class_id.str());
  }
  if (record.history.size() != revisions.size()) {
    return refuse(ErrorCode::CorruptStore,
                  "restored class history has " + std::to_string(record.history.size()) + " entries for " +
                      std::to_string(revisions.size()) + " revisions",
                  record.class_id.str());
  }
  StoredRevision previous;
  bool have_previous = false;
  std::vector<Digest> digests;

  for (std::size_t index = 0; index < revisions.size(); ++index) {
    const StoredRevision& stored = revisions[index];
    if (stored.content.class_id != record.class_id) {
      return refuse(ErrorCode::CorruptStore, "stored revision belongs to a different class", record.class_id.str());
    }
    if (stored.digest.is_null()) {
      return refuse(ErrorCode::CorruptStore, "stored revision has a null digest", record.class_id.str());
    }
    const Status validation = stored.content.validate();
    if (!validation.ok()) {
      return validation;
    }
    const auto recomputed = revision_frame_digest(stored.content);
    if (!recomputed.ok()) {
      return recomputed.status();
    }
    if (recomputed.value() != stored.digest) {
      return refuse(ErrorCode::CorruptStore,
                    "stored revision " + std::to_string(stored.content.generation.value()) + "." +
                        std::to_string(stored.content.revision.value()) + " digest does not match its content",
                    record.class_id.str());
    }
    if (stored.content.revision.is_zero() || stored.content.generation.is_zero()) {
      return refuse(ErrorCode::CorruptStore, "stored revision has a zero generation or revision", record.class_id.str());
    }
    if (stored.content.revision.value() == 1) {
      if (!stored.content.lineage.is_genesis()) {
        return refuse(ErrorCode::CorruptStore,
                      "the first revision of a generation must be a genesis revision", record.class_id.str());
      }
      if (have_previous) {
        const auto expected = previous.content.generation.next();
        if (!expected.ok()) {
          return expected.status();
        }
        if (stored.content.generation != expected.value()) {
          return refuse(ErrorCode::CorruptStore,
                        "generation numbers must be contiguous: expected generation " +
                            std::to_string(expected.value().value()),
                        record.class_id.str());
        }
      } else if (stored.content.generation.value() != 1) {
        return refuse(ErrorCode::CorruptStore, "the first generation of a class must be generation 1",
                      record.class_id.str());
      }
    } else {
      if (!have_previous) {
        return refuse(ErrorCode::CorruptStore, "a class history cannot start above revision 1", record.class_id.str());
      }
      const auto expected = previous.content.revision.next();
      if (!expected.ok()) {
        return expected.status();
      }
      if (stored.content.generation != previous.content.generation ||
          stored.content.revision != expected.value()) {
        return refuse(ErrorCode::CorruptStore,
                      "revision numbers must be contiguous within a generation", record.class_id.str());
      }
      if (stored.content.lineage.is_genesis()) {
        return refuse(ErrorCode::CorruptStore, "only the first revision of a generation is a genesis revision",
                      record.class_id.str());
      }
      RevisionRef expected_predecessor;
      expected_predecessor.generation = previous.content.generation;
      expected_predecessor.revision = previous.content.revision;
      expected_predecessor.digest = previous.digest;
      if (stored.content.lineage.predecessor() != expected_predecessor) {
        return refuse(ErrorCode::CorruptStore, "lineage does not name the immediately preceding revision",
                      record.class_id.str());
      }
    }
    const ClassHistoryEntry& entry = record.history[index];
    RevisionRef entry_reference;
    entry_reference.generation = stored.content.generation;
    entry_reference.revision = stored.content.revision;
    entry_reference.digest = stored.digest;
    if (entry.reference != entry_reference) {
      return refuse(ErrorCode::CorruptStore, "class history does not match the stored revisions",
                    record.class_id.str());
    }
    if (entry.generation_start != (stored.content.revision.value() == 1)) {
      return refuse(ErrorCode::CorruptStore, "class history generation markers do not match the stored revisions",
                    record.class_id.str());
    }
    digests.push_back(stored.digest);
    previous = stored;
    have_previous = true;
  }

  const StoredRevision& last = revisions.back();
  if (last.content.generation != record.generation || last.content.revision != record.tip ||
      last.digest != record.tip_digest) {
    return refuse(ErrorCode::CorruptStore, "class tip does not match the last stored revision", record.class_id.str());
  }
  return Status::success();
}

Status Registry::restore_class(const ClassRecord& record, const std::vector<StoredRevision>& revisions) {
  const Status verification = verify_class_payload(record, revisions);
  if (!verification.ok()) {
    return verification;
  }
  if (slots_.find(record.class_id) != slots_.end()) {
    return refuse(ErrorCode::DuplicateIdentity, "class is already present in this registry", record.class_id.str());
  }
  for (const StoredRevision& stored : revisions) {
    if (revision_index_.find(stored.digest) != revision_index_.end()) {
      return refuse(ErrorCode::DuplicateIdentity, "revision digest is already indexed", record.class_id.str());
    }
  }

  Slot slot;
  slot.record = record;
  slot.revisions = revisions;
  const auto inserted = slots_.emplace(record.class_id, std::move(slot));
  if (!inserted.second) {
    return refuse(ErrorCode::DuplicateIdentity, "class is already present in this registry", record.class_id.str());
  }
  for (std::size_t index = 0; index < revisions.size(); ++index) {
    const Status indexed = index_revision(record.class_id, index);
    if (!indexed.ok()) {
      slots_.erase(record.class_id);
      for (const StoredRevision& stored : revisions) {
        revision_index_.erase(stored.digest);
      }
      return indexed;
    }
  }
  return Status::success();
}

Result<ClassSnapshot> Registry::snapshot_class(const ClassId& class_id) const {
  ClassSnapshot snapshot;
  snapshot.class_id = class_id;
  const Slot* slot = find_slot(class_id);
  if (slot == nullptr) {
    snapshot.present = false;
    return Result<ClassSnapshot>::success(std::move(snapshot));
  }
  snapshot.present = true;
  snapshot.record = slot->record;
  snapshot.revisions = slot->revisions;
  return Result<ClassSnapshot>::success(std::move(snapshot));
}

Status Registry::restore_snapshot(const ClassSnapshot& snapshot) {
  const auto found = slots_.find(snapshot.class_id);
  if (found != slots_.end()) {
    for (const StoredRevision& stored : found->second.revisions) {
      revision_index_.erase(stored.digest);
    }
    slots_.erase(found);
  }
  if (!snapshot.present) {
    return Status::success();
  }
  Slot slot;
  slot.record = snapshot.record;
  slot.revisions = snapshot.revisions;
  slots_.emplace(snapshot.class_id, std::move(slot));
  for (std::size_t index = 0; index < snapshot.revisions.size(); ++index) {
    const Status indexed = index_revision(snapshot.class_id, index);
    if (!indexed.ok()) {
      return indexed;
    }
  }
  return Status::success();
}

Status Registry::validate_all() const {
  for (const auto& entry : slots_) {
    const Status validation = verify_class_payload(entry.second.record, entry.second.revisions);
    if (!validation.ok()) {
      return validation;
    }
  }
  return Status::success();
}

}  // namespace scr
