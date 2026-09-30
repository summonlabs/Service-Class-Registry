#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "scr/canonical.hpp"
#include "scr/composition.hpp"
#include "scr/contradiction.hpp"
#include "scr/revision.hpp"

namespace scr {

// ---------------------------------------------------------------------------
// Lifecycle and binding states
// ---------------------------------------------------------------------------

// Lifecycle state of a class. It is index state, not revision content: retiring
// a class never changes the digest of any revision it published, so historical
// decisions stay verifiable.
enum class ClassState : std::uint8_t { Active = 1, Retired = 2 };
const char* to_string(ClassState state) noexcept;

// Outcome of resolving a consumer binding against the authoritative index.
enum class BindingState : std::uint8_t {
  Fresh = 1,
  Superseded = 2,
  Retired = 3,
  GenerationSuperseded = 4,
  UnknownClass = 5,
  UnknownRevision = 6,
  DigestMismatch = 7,
};
const char* to_string(BindingState state) noexcept;

struct BindingResolution {
  BindingState state = BindingState::UnknownClass;
  ClassBinding binding;
  bool tip_present = false;
  RevisionRef authoritative_tip;
  std::string detail;
};

// ---------------------------------------------------------------------------
// Index records
// ---------------------------------------------------------------------------

struct StoredRevision {
  ClassRevision content;
  Digest digest;
  Timestamp published_at;
};

struct RevisionRecord {
  ClassRevision content;
  Digest digest;
  Timestamp published_at;
  ClassState state = ClassState::Active;
};

struct ClassHistoryEntry {
  RevisionRef reference;
  Timestamp published_at;
  // True when this publication started a generation (revision 1).
  bool generation_start = false;
};

struct ClassRecord {
  ClassId class_id;
  Generation generation;
  ClassState state = ClassState::Active;
  Revision tip;
  Digest tip_digest;
  Timestamp published_at;   // publication time of the current tip
  Timestamp retired_at;
  BoundedText retire_reason;
  std::vector<ClassHistoryEntry> history;  // ascending (generation, revision)
};

struct PublishOptions {
  // Explicit re-founding: starts generation + 1 at revision 1 with genesis
  // lineage. Refused unless the caller asks for it.
  bool allow_new_generation = false;
  // A retired parent class may not be composed by a new revision.
  bool allow_retired_parents = false;
  Timestamp at;
  std::size_t max_depth = 32;
  std::size_t max_revisions = 4096;
};

struct PublishOutcome {
  RevisionRef reference;
  bool idempotent = false;
  std::vector<Diagnostic> advisories;
};

struct RetireOutcome {
  ClassId class_id;
  RevisionRef tip;
  Timestamp retired_at;
  // Retirement is idempotent for lost-response safety: retiring an already
  // retired class succeeds without changing the recorded retirement.
  bool already_retired = false;
};

// ---------------------------------------------------------------------------
// Comparison and explanation
// ---------------------------------------------------------------------------

enum class RevisionRelation : std::uint8_t {
  Identical = 1,
  FromAncestorOfTo = 2,
  ToAncestorOfFrom = 3,
  DifferentGeneration = 4,
};
const char* to_string(RevisionRelation relation) noexcept;

enum class DeltaKind : std::uint8_t {
  Added = 1,
  Removed = 2,
  ModalityChanged = 3,
  ValueChanged = 4,
};

struct ObligationDelta {
  ObligationKey key = ObligationKey::AvailabilityTargetPpm;
  DeltaKind kind = DeltaKind::Added;
  bool from_present = false;
  Modality from_modality = Modality::Unspecified;
  std::uint64_t from_value = 0;
  bool to_present = false;
  Modality to_modality = Modality::Unspecified;
  std::uint64_t to_value = 0;
  // stricter | weaker | equivalent | incomparable | not-applicable
  const char* strictness_effect = "not-applicable";
};

struct CompareReport {
  ClassId class_id;
  RevisionRef from;
  RevisionRef to;
  RevisionRelation relation = RevisionRelation::Identical;
  bool identical_content = false;
  std::vector<ObligationDelta> declared;   // declared obligations, ascending key
  std::vector<ObligationDelta> effective;  // flattened semantics, ascending key
  std::vector<std::string> reference_changes;
  std::vector<std::string> composition_changes;
  std::vector<std::string> metadata_changes;
};

struct ObligationExplanation {
  ObligationKey key = ObligationKey::AvailabilityTargetPpm;
  Modality modality = Modality::Unspecified;
  std::uint64_t value = 0;
  bool inherited = false;
  std::vector<Provenance> provenance;
  std::vector<Suppression> suppressed;
};

struct ClassExplanation {
  ClassId class_id;
  ClassState state = ClassState::Active;
  bool tip_present = false;
  RevisionRef authoritative;
  Timestamp published_at;
  Timestamp retired_at;
  std::string retire_reason;
  std::vector<ObligationExplanation> obligations;
  std::vector<ExternalReference> references;
  std::vector<CompositionRef> composes;
  std::vector<Diagnostic> advisories;
  std::vector<RuleViolation> rules;          // evaluated over the effective set
  std::vector<RevisionRef> resolution_order; // deterministic composition order
  std::vector<ClassHistoryEntry> history;
};

// ---------------------------------------------------------------------------
// Registry
//
// The registry is the in-memory authoritative index. It is single threaded per
// instance by design: one registry is owned by one thread, and cross-process
// exclusion is enforced by the durable store lock. Published revisions are
// immutable; the digest of a revision is a function of its declared semantics
// only.
// ---------------------------------------------------------------------------

// Opaque capture of one class slot, used by the durable store to roll back an
// in-memory publication whose durable commit failed.
struct ClassSnapshot {
  ClassId class_id;
  bool present = false;
  ClassRecord record;
  std::vector<StoredRevision> revisions;
};

class Registry {
 public:
  Registry() = default;

  const AuthorityId& authority() const noexcept { return authority_; }
  void set_authority(AuthorityId authority) { authority_ = std::move(authority); }

  Epoch epoch() const noexcept { return epoch_; }
  void set_epoch(Epoch epoch) noexcept { epoch_ = epoch; }

  // Validates, canonicalizes a copy, resolves composition, evaluates the rule
  // set, and publishes. Refusals carry the primary failure plus every other
  // violation that was observed.
  Result<PublishOutcome> publish(const ClassRevision& content, const PublishOptions& options = PublishOptions{});

  Result<BindingResolution> resolve(const ClassBinding& binding) const;

  Result<RevisionRecord> get(const RevisionRef& reference) const;
  Result<RevisionRecord> get(const ClassId& class_id, Generation generation, Revision revision) const;
  Result<ClassRecord> class_record(const ClassId& class_id) const;

  Result<FlattenedSemantics> flatten(const RevisionRef& reference) const;
  Result<FlattenedSemantics> flatten_authoritative(const ClassId& class_id) const;

  Result<CompareReport> compare(const RevisionRef& from, const RevisionRef& to) const;
  Result<ClassExplanation> explain(const ClassId& class_id) const;
  Result<ClassExplanation> explain(const RevisionRef& reference) const;

  Result<RetireOutcome> retire(const ClassId& class_id, const BoundedText& reason, const Timestamp& at);

  std::vector<ClassRecord> class_records() const;
  std::size_t class_count() const noexcept { return slots_.size(); }
  std::size_t revision_count() const noexcept;
  bool empty() const noexcept { return slots_.empty(); }

  // Store integration: exact history of one class, ascending.
  Result<std::vector<StoredRevision>> stored_history(const ClassId& class_id) const;

  // Recovery: rebuilds one class from durable records. Verifies identity,
  // digest, contiguous revisions, lineage links, tip, and retirement shape.
  // Fails closed: nothing is imported when anything does not verify.
  Status restore_class(const ClassRecord& record, const std::vector<StoredRevision>& revisions);

  Result<ClassSnapshot> snapshot_class(const ClassId& class_id) const;
  Status restore_snapshot(const ClassSnapshot& snapshot);

  Status validate_all() const;

 private:
  struct Slot {
    ClassRecord record;
    std::vector<StoredRevision> revisions;
  };

  std::map<ClassId, Slot> slots_;
  // Global digest index: a digest is unique across the registry because a
  // revision's content includes its class identity.
  std::map<Digest, std::pair<ClassId, std::size_t>> revision_index_;
  AuthorityId authority_;
  Epoch epoch_;

  const Slot* find_slot(const ClassId& class_id) const;
  Result<StoredRevision> find_stored(const RevisionRef& reference) const;
  Result<ResolvedRevision> resolve_parent(const RevisionRef& reference) const;
  Status index_revision(const ClassId& class_id, std::size_t position);
  // Verifies a class payload (identity, digests, contiguity, lineage, tip,
  // retirement shape) without touching the index.
  Status verify_class_payload(const ClassRecord& record, const std::vector<StoredRevision>& revisions) const;
};

}  // namespace scr
