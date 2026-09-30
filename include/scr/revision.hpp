#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "scr/obligation.hpp"
#include "scr/result.hpp"
#include "scr/types.hpp"

namespace scr {

// External references: form validation plus optional exact content binding.
// The registry never dereferences a target and never takes over its authority.

enum class ReferenceKind : std::uint8_t {
  RequirementSet = 1,
  PolicyPredicate = 2,
  EntitlementProfile = 3,
  EvidenceSource = 4,
};

const char* to_string(ReferenceKind kind) noexcept;
Result<ReferenceKind> parse_reference_kind(std::string_view text);

struct ExternalReference {
  ReferenceKind kind = ReferenceKind::RequirementSet;
  std::string target;
  // Null digest means "explicitly unbound": the reference is named but no
  // content binding was supplied, so staleness cannot be detected for it.
  Digest digest;
  // Empty authority means "unspecified authority".
  AuthorityId authority;
  BoundedText note;

  static Result<ExternalReference> create(ReferenceKind kind, std::string target, Digest digest,
                                          AuthorityId authority, BoundedText note);

  bool is_bound() const noexcept { return !digest.is_null(); }

  std::string to_string() const;

  friend bool operator==(const ExternalReference& a, const ExternalReference& b) noexcept {
    return a.kind == b.kind && a.target == b.target && a.digest == b.digest && a.authority == b.authority &&
           a.note == b.note;
  }
  friend bool operator!=(const ExternalReference& a, const ExternalReference& b) noexcept { return !(a == b); }
};

// Composition edge: an exact parent revision, never "latest".
struct CompositionRef {
  ClassId parent;
  RevisionRef revision;

  static Result<CompositionRef> create(ClassId parent, RevisionRef revision);

  std::string to_string() const;

  friend bool operator==(const CompositionRef& a, const CompositionRef& b) noexcept {
    return a.parent == b.parent && a.revision == b.revision;
  }
  friend bool operator!=(const CompositionRef& a, const CompositionRef& b) noexcept { return !(a == b); }
};

// Explicit history: a genesis revision starts a generation, every other
// revision names its exact predecessor (generation, revision, digest).
class Lineage {
 public:
  static Lineage genesis() noexcept { return Lineage(); }

  static Result<Lineage> from_predecessor(RevisionRef predecessor);

  bool is_genesis() const noexcept { return genesis_; }
  const RevisionRef& predecessor() const noexcept { return predecessor_; }

  friend bool operator==(const Lineage& a, const Lineage& b) noexcept {
    return a.genesis_ == b.genesis_ && (a.genesis_ || a.predecessor_ == b.predecessor_);
  }
  friend bool operator!=(const Lineage& a, const Lineage& b) noexcept { return !(a == b); }

 private:
  bool genesis_ = true;
  RevisionRef predecessor_;
};

// Bounded, validated UTF-8 metadata. Never authority.
struct Metadata {
  BoundedText title;
  BoundedText summary;
  BoundedText owner;
  BoundedText documentation;

  friend bool operator==(const Metadata& a, const Metadata& b) noexcept {
    return a.title == b.title && a.summary == b.summary && a.owner == b.owner && a.documentation == b.documentation;
  }
  friend bool operator!=(const Metadata& a, const Metadata& b) noexcept { return !(a == b); }
};

// The digestible semantics of a revision. It deliberately contains no
// wall-clock value, no lifecycle state, and no store identity: the digest
// depends only on declared semantics, which is what makes idempotent replay and
// content-addressed storage exact. Lifecycle state and publication time live in
// the store index, outside the digest, so retiring a class cannot invalidate an
// existing binding.
inline constexpr std::size_t kMaxReferencesPerRevision = 64;
inline constexpr std::size_t kMaxCompositionsPerRevision = 64;

struct ClassRevision {
  ClassId class_id;
  Generation generation;
  Revision revision;
  Lineage lineage;
  ObligationSet obligations;
  std::vector<ExternalReference> references;  // canonical order required
  std::vector<CompositionRef> composes;       // canonical order required
  Metadata metadata;

  // Structural validation with no registry context: identity, counters,
  // lineage shape, obligation shape (including at least one Required
  // obligation), entry bounds, duplicate identities, canonical ordering.
  Status validate() const;
};

bool reference_less(const ExternalReference& a, const ExternalReference& b) noexcept;
bool composition_less(const CompositionRef& a, const CompositionRef& b) noexcept;

// Sorts references and compositions into canonical order. Fails with
// DuplicateIdentity when two entries share (kind, target) or parent class.
Status canonicalize(ClassRevision& revision);

Result<ClassRevision> make_revision(ClassId class_id, Generation generation, Revision revision, Lineage lineage,
                                    ObligationSet obligations, std::vector<ExternalReference> references,
                                    std::vector<CompositionRef> composes, Metadata metadata);

}  // namespace scr
