#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "scr/result.hpp"
#include "scr/types.hpp"

namespace scr {

// ---------------------------------------------------------------------------
// Obligation keys
//
// A service class is never an opaque marketing label: it is a set of typed
// obligations drawn from this closed registry. Adding a key is a wire-format
// change; existing numeric key values are never reused or renumbered.
// ---------------------------------------------------------------------------

enum class ObligationKey : std::uint16_t {
  AvailabilityTargetPpm = 1,
  AvailabilityMaxAnnualDowntimeSeconds = 2,
  RedundancyTopology = 3,
  RedundancyMinimumIndependentFaultDomains = 4,
  RedundancyConcurrentFaultTolerance = 5,
  RedundancyFailoverMode = 6,
  RedundancyMaximumFailoverSeconds = 7,
  MaintenanceMode = 8,
  MaintenanceMaximumWindowSeconds = 9,
  MaintenanceMinimumNoticeSeconds = 10,
  MaintenanceConcurrencyLimit = 11,
  RecoveryTimeObjectiveSeconds = 12,
  RecoveryPointObjectiveSeconds = 13,
  RecoveryRestoreMode = 14,
  PowerFeedCount = 15,
  PowerPathIndependence = 16,
  PowerAutonomySeconds = 17,
  PowerTransferMode = 18,
  PowerMaximumTransferSeconds = 19,
  CoolingMode = 20,
  CoolingIndependentPaths = 21,
  CoolingMaximumAmbientMillicelsius = 22,
  CoolingAutonomySeconds = 23,
  PlacementMinimumIsolationDomains = 24,
  PlacementTenantSeparation = 25,
  PlacementGeographicDiversity = 26,
  OperationsMonitoringIntervalSeconds = 27,
  OperationsChangeControl = 28,
  OperationsOnCallResponseSeconds = 29,
  OperationsIncidentNotificationSeconds = 30,
};

inline constexpr std::size_t kObligationKeyCount = 30;

// Modality is the strength with which an obligation is asserted.
//   Unspecified        - explicitly unknown/undetermined. Carries value 0 by
//                        construction; it is preserved, never treated as absent
//                        and never treated as a default.
//   PermittedDegraded  - degraded operation to this level is permitted.
//   Preferred          - declared preference, not a hard requirement.
//   Required           - hard requirement.
enum class Modality : std::uint8_t {
  Unspecified = 0,
  PermittedDegraded = 1,
  Preferred = 2,
  Required = 3,
};

const char* to_string(Modality modality) noexcept;
Result<Modality> parse_modality(std::string_view text);

enum class ValueKind : std::uint8_t { UnsignedInteger = 0, Enumerated = 1 };

// Strictness tells the merge lattice which direction is "stricter" for a key.
// Incomparable keys only merge when the values are equal.
enum class Strictness : std::uint8_t {
  HigherIsStricter = 0,
  LowerIsStricter = 1,
  Incomparable = 2,
};

struct EnumName {
  std::uint64_t value;
  const char* name;
};

struct KeyDescriptor {
  ObligationKey key;
  const char* name;          // canonical text-format name, e.g. "redundancy.topology"
  ValueKind kind;
  Strictness strictness;
  std::uint64_t min_value;   // inclusive; for enumerated keys: lowest enumerator
  std::uint64_t max_value;   // inclusive; for enumerated keys: highest enumerator
  const EnumName* enum_names;
  std::size_t enum_count;
};

const KeyDescriptor& describe(ObligationKey key);
// Returns nullptr when the name is unknown.
const KeyDescriptor* find_key(std::string_view name);
// Stable iteration order: ascending numeric key value.
const KeyDescriptor& key_at(std::size_t index);
bool is_valid_obligation_key(std::uint16_t raw) noexcept;

// ---------------------------------------------------------------------------
// Typed enumerations used by obligations
// ---------------------------------------------------------------------------

enum class RedundancyTopology : std::uint64_t {
  Unspecified = 0,
  None = 1,
  N = 2,
  NPlusOne = 3,
  NPlusTwo = 4,
  TwoN = 5,
  TwoNPlusOne = 6,
  Distributed = 7,
};

enum class FailoverMode : std::uint64_t { Unspecified = 0, None = 1, Manual = 2, Automatic = 3 };
enum class MaintenanceMode : std::uint64_t {
  Unspecified = 0,
  None = 1,
  Offline = 2,
  Rolling = 3,
  Online = 4,
  ConcurrentMaintainable = 5,
};
enum class RestoreMode : std::uint64_t { Unspecified = 0, None = 1, Cold = 2, Warm = 3, Hot = 4, Continuous = 5 };
enum class PathIndependence : std::uint64_t {
  Unspecified = 0,
  Single = 1,
  DualIndependent = 2,
  MultipleIndependent = 3,
};
enum class TransferMode : std::uint64_t { Unspecified = 0, None = 1, Manual = 2, Automatic = 3 };
enum class CoolingMode : std::uint64_t {
  Unspecified = 0,
  None = 1,
  Passive = 2,
  Mechanical = 3,
  RedundantMechanical = 4,
};
enum class TenantSeparation : std::uint64_t { Unspecified = 0, None = 1, Logical = 2, Physical = 3 };
enum class GeographicDiversity : std::uint64_t {
  Unspecified = 0,
  None = 1,
  SameCampus = 2,
  SameRegion = 3,
  MultiRegion = 4,
};
enum class ChangeControl : std::uint64_t { Unspecified = 0, None = 1, Standard = 2, Strict = 3 };

// ---------------------------------------------------------------------------
// Obligation
// ---------------------------------------------------------------------------

// An obligation is (key, modality, value). The value is a raw unsigned integer
// whose interpretation is fixed by the key descriptor; every construction path
// validates kind, range, enumerator membership, and the Unspecified => 0 rule.
class Obligation {
 public:
  Obligation() = default;

  static Result<Obligation> create(ObligationKey key, Modality modality, std::uint64_t value);

  ObligationKey key() const noexcept { return key_; }
  Modality modality() const noexcept { return modality_; }
  std::uint64_t value() const noexcept { return value_; }

  // Canonical text-format rendering of the value, e.g. "n+1" or "99990000".
  std::string value_text() const;

  friend bool operator==(const Obligation& a, const Obligation& b) noexcept {
    return a.key_ == b.key_ && a.modality_ == b.modality_ && a.value_ == b.value_;
  }
  friend bool operator!=(const Obligation& a, const Obligation& b) noexcept { return !(a == b); }

 private:
  ObligationKey key_ = ObligationKey::AvailabilityTargetPpm;
  Modality modality_ = Modality::Unspecified;
  std::uint64_t value_ = 0;
};

// The merge lattice: returns the dominating modality, or Incomparable when the
// two modalities cannot be ordered (which cannot happen with this enumeration,
// but the API states the total order explicitly rather than relying on the
// numeric values of the enum).
enum class MergeOrder { LeftDominates, RightDominates, Equal };

MergeOrder compare_modality(Modality a, Modality b) noexcept;

// Compares two values of the same key by the key's strictness relation.
// Returns -1 when a is stricter, 0 when equal, +1 when b is stricter, and
// reports Incomparable when the key is Incomparable and values differ.
struct StrictnessComparison {
  enum class Relation { AStricter, Equal, BStricter, Incomparable } relation;
};
StrictnessComparison::Relation compare_strictness(ObligationKey key, std::uint64_t a, std::uint64_t b) noexcept;

// Canonical obligation set: at most one obligation per key, iterated in
// ascending key order regardless of insertion order.
class ObligationSet {
 public:
  using Container = std::map<ObligationKey, Obligation>;

  // Refuses a second declaration of the same key (DuplicateObligation), even
  // when it is byte-identical, so that canonicalization is unambiguous.
  VoidResult add(const Obligation& obligation);

  Result<Obligation> get(ObligationKey key) const;

  const Container& items() const noexcept { return items_; }
  bool empty() const noexcept { return items_.empty(); }
  std::size_t size() const noexcept { return items_.size(); }

  // True when the key is present at all (including with Unspecified modality).
  bool contains(ObligationKey key) const noexcept { return items_.find(key) != items_.end(); }

  friend bool operator==(const ObligationSet& a, const ObligationSet& b) noexcept { return a.items_ == b.items_; }
  friend bool operator!=(const ObligationSet& a, const ObligationSet& b) noexcept { return !(a == b); }

 private:
  Container items_;
};

inline constexpr std::size_t kMaxObligationsPerRevision = kObligationKeyCount;

}  // namespace scr
