#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "scr/revision.hpp"
#include "scr/result.hpp"

namespace scr {

// ---------------------------------------------------------------------------
// Composition and flattening
//
// A revision may compose exact parent revisions. Composition is a conjunction:
// the composite must satisfy every parent obligation as well as its own, so
// parents merge by lattice join (stricter wins, dominance is recorded, an
// incomparable disagreement is refused). The target revision's own obligations
// may strengthen an inherited required obligation but never weaken it.
//
// Resolution is provably acyclic: the graph is walked depth first with an
// explicit in-progress set, and re-entering a node that is still being resolved
// is refused as CycleDetected with the exact cycle path. The walk is bounded in
// depth and in the number of resolved revisions.
// ---------------------------------------------------------------------------

struct Provenance {
  ClassId class_id;
  RevisionRef revision;
  Modality modality = Modality::Unspecified;
  std::uint64_t value = 0;
  // True only for a declaration that appears literally in the revision being
  // explained. Inherited declarations always carry direct == false.
  bool direct = false;
};

struct Suppression {
  ClassId class_id;
  RevisionRef revision;
  Modality modality = Modality::Unspecified;
  std::uint64_t value = 0;
  // Stable reason identifier: dominated-by-modality, dominated-by-strictness,
  // explicit-unspecified-does-not-override, strengthened-by-target.
  std::string reason;
};

struct EffectiveObligation {
  ObligationKey key = ObligationKey::AvailabilityTargetPpm;
  Modality modality = Modality::Unspecified;
  std::uint64_t value = 0;
  bool inherited = false;
  std::vector<Provenance> provenance;  // deterministic order
  std::vector<Suppression> suppressed; // deterministic order
};

struct FlattenedSemantics {
  std::vector<EffectiveObligation> obligations;  // ascending key order
  std::vector<Diagnostic> advisories;            // deterministic order
  std::vector<RevisionRef> resolution_order;     // parents first, deduplicated
  std::size_t resolved_revisions = 0;
};

struct ResolvedRevision {
  ClassRevision content;
  // Lifecycle state of the class at resolution time. A retired parent is still
  // resolvable for historical explanation but is refused for a new publication.
  bool retired = false;
};

using RevisionResolver = std::function<Result<ResolvedRevision>(const RevisionRef&)>;

struct FlattenOptions {
  // Publication path: a retired parent class may not be composed by a new
  // revision. Historical explanation keeps it true.
  bool allow_retired_parents = true;
  std::size_t max_depth = 32;
  std::size_t max_revisions = 4096;
};

// Flattens a revision whose exact identity (including its content digest) is
// known. Provenance entries then carry the digest that a consumer can bind to.
Result<FlattenedSemantics> flatten_revision(const ClassRevision& content, const RevisionRef& identity,
                                            const RevisionResolver& resolver,
                                            const FlattenOptions& options = FlattenOptions{});

// Flattens a revision whose digest is not known to the caller. Provenance
// entries carry a null digest, so this form is for structural checks and tests
// rather than for bindings.
Result<FlattenedSemantics> flatten_revision(const ClassRevision& content, const RevisionResolver& resolver,
                                            const FlattenOptions& options = FlattenOptions{});

// Looks up an effective obligation by key. UnknownField when absent.
Result<EffectiveObligation> find_effective(const FlattenedSemantics& semantics, ObligationKey key);

// Builds the canonical obligation set of the effective values, which is what
// the cross-key rule engine evaluates.
ObligationSet effective_obligation_set(const FlattenedSemantics& semantics);

}  // namespace scr
