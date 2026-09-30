#include "scr/obligation.hpp"

#include <algorithm>
#include <array>

namespace scr {
namespace {

// Enumerator tables. Names are the canonical text-format spellings and are part
// of the public contract; they are never renamed for an existing numeric value.
constexpr EnumName kRedundancyTopologyNames[] = {
    {0, "unspecified"}, {1, "none"},      {2, "n"},        {3, "n+1"},
    {4, "n+2"},         {5, "2n"},        {6, "2n+1"},     {7, "distributed"},
};
constexpr EnumName kFailoverModeNames[] = {
    {0, "unspecified"}, {1, "none"}, {2, "manual"}, {3, "automatic"},
};
constexpr EnumName kMaintenanceModeNames[] = {
    {0, "unspecified"}, {1, "none"},  {2, "offline"},
    {3, "rolling"},     {4, "online"}, {5, "concurrent-maintainable"},
};
constexpr EnumName kRestoreModeNames[] = {
    {0, "unspecified"}, {1, "none"}, {2, "cold"}, {3, "warm"}, {4, "hot"}, {5, "continuous"},
};
constexpr EnumName kPathIndependenceNames[] = {
    {0, "unspecified"}, {1, "single"}, {2, "dual-independent"}, {3, "multiple-independent"},
};
constexpr EnumName kTransferModeNames[] = {
    {0, "unspecified"}, {1, "none"}, {2, "manual"}, {3, "automatic"},
};
constexpr EnumName kCoolingModeNames[] = {
    {0, "unspecified"}, {1, "none"}, {2, "passive"}, {3, "mechanical"}, {4, "redundant-mechanical"},
};
constexpr EnumName kTenantSeparationNames[] = {
    {0, "unspecified"}, {1, "none"}, {2, "logical"}, {3, "physical"},
};
constexpr EnumName kGeographicDiversityNames[] = {
    {0, "unspecified"}, {1, "none"}, {2, "same-campus"}, {3, "same-region"}, {4, "multi-region"},
};
constexpr EnumName kChangeControlNames[] = {
    {0, "unspecified"}, {1, "none"}, {2, "standard"}, {3, "strict"},
};

template <std::size_t N>
constexpr std::size_t count_of(const EnumName (&)[N]) noexcept {
  return N;
}

// The descriptor table. Index 0 is a reserved placeholder so that the table can
// be indexed directly by the numeric obligation key.
constexpr KeyDescriptor kDescriptors[] = {
    {static_cast<ObligationKey>(0), "reserved", ValueKind::UnsignedInteger, Strictness::Incomparable, 0, 0, nullptr, 0},
    {ObligationKey::AvailabilityTargetPpm, "availability.target_ppm", ValueKind::UnsignedInteger,
     Strictness::HigherIsStricter, 0, 1000000, nullptr, 0},
    {ObligationKey::AvailabilityMaxAnnualDowntimeSeconds, "availability.max_annual_downtime_seconds",
     ValueKind::UnsignedInteger, Strictness::LowerIsStricter, 0, 31536000, nullptr, 0},
    {ObligationKey::RedundancyTopology, "redundancy.topology", ValueKind::Enumerated, Strictness::Incomparable, 0, 7,
     kRedundancyTopologyNames, count_of(kRedundancyTopologyNames)},
    {ObligationKey::RedundancyMinimumIndependentFaultDomains, "redundancy.minimum_independent_fault_domains",
     ValueKind::UnsignedInteger, Strictness::HigherIsStricter, 0, 64, nullptr, 0},
    {ObligationKey::RedundancyConcurrentFaultTolerance, "redundancy.concurrent_fault_tolerance",
     ValueKind::UnsignedInteger, Strictness::HigherIsStricter, 0, 63, nullptr, 0},
    {ObligationKey::RedundancyFailoverMode, "redundancy.failover_mode", ValueKind::Enumerated,
     Strictness::Incomparable, 0, 3, kFailoverModeNames, count_of(kFailoverModeNames)},
    {ObligationKey::RedundancyMaximumFailoverSeconds, "redundancy.maximum_failover_seconds",
     ValueKind::UnsignedInteger, Strictness::LowerIsStricter, 0, 86400, nullptr, 0},
    {ObligationKey::MaintenanceMode, "maintenance.mode", ValueKind::Enumerated, Strictness::Incomparable, 0, 5,
     kMaintenanceModeNames, count_of(kMaintenanceModeNames)},
    {ObligationKey::MaintenanceMaximumWindowSeconds, "maintenance.maximum_window_seconds",
     ValueKind::UnsignedInteger, Strictness::LowerIsStricter, 0, 2592000, nullptr, 0},
    {ObligationKey::MaintenanceMinimumNoticeSeconds, "maintenance.minimum_notice_seconds",
     ValueKind::UnsignedInteger, Strictness::HigherIsStricter, 0, 31536000, nullptr, 0},
    {ObligationKey::MaintenanceConcurrencyLimit, "maintenance.concurrency_limit", ValueKind::UnsignedInteger,
     Strictness::HigherIsStricter, 0, 1024, nullptr, 0},
    {ObligationKey::RecoveryTimeObjectiveSeconds, "recovery.time_objective_seconds", ValueKind::UnsignedInteger,
     Strictness::LowerIsStricter, 0, 31536000, nullptr, 0},
    {ObligationKey::RecoveryPointObjectiveSeconds, "recovery.point_objective_seconds", ValueKind::UnsignedInteger,
     Strictness::LowerIsStricter, 0, 31536000, nullptr, 0},
    {ObligationKey::RecoveryRestoreMode, "recovery.restore_mode", ValueKind::Enumerated, Strictness::Incomparable, 0, 5,
     kRestoreModeNames, count_of(kRestoreModeNames)},
    {ObligationKey::PowerFeedCount, "power.feed_count", ValueKind::UnsignedInteger, Strictness::HigherIsStricter, 0, 64,
     nullptr, 0},
    {ObligationKey::PowerPathIndependence, "power.path_independence", ValueKind::Enumerated, Strictness::Incomparable, 0,
     3, kPathIndependenceNames, count_of(kPathIndependenceNames)},
    {ObligationKey::PowerAutonomySeconds, "power.autonomy_seconds", ValueKind::UnsignedInteger,
     Strictness::HigherIsStricter, 0, 86400, nullptr, 0},
    {ObligationKey::PowerTransferMode, "power.transfer_mode", ValueKind::Enumerated, Strictness::Incomparable, 0, 3,
     kTransferModeNames, count_of(kTransferModeNames)},
    {ObligationKey::PowerMaximumTransferSeconds, "power.maximum_transfer_seconds", ValueKind::UnsignedInteger,
     Strictness::LowerIsStricter, 0, 3600, nullptr, 0},
    {ObligationKey::CoolingMode, "cooling.mode", ValueKind::Enumerated, Strictness::Incomparable, 0, 4,
     kCoolingModeNames, count_of(kCoolingModeNames)},
    {ObligationKey::CoolingIndependentPaths, "cooling.independent_paths", ValueKind::UnsignedInteger,
     Strictness::HigherIsStricter, 0, 64, nullptr, 0},
    {ObligationKey::CoolingMaximumAmbientMillicelsius, "cooling.maximum_ambient_millicelsius",
     ValueKind::UnsignedInteger, Strictness::LowerIsStricter, 0, 60000, nullptr, 0},
    {ObligationKey::CoolingAutonomySeconds, "cooling.autonomy_seconds", ValueKind::UnsignedInteger,
     Strictness::HigherIsStricter, 0, 86400, nullptr, 0},
    {ObligationKey::PlacementMinimumIsolationDomains, "placement.minimum_isolation_domains",
     ValueKind::UnsignedInteger, Strictness::HigherIsStricter, 0, 1024, nullptr, 0},
    {ObligationKey::PlacementTenantSeparation, "placement.tenant_separation", ValueKind::Enumerated,
     Strictness::Incomparable, 0, 3, kTenantSeparationNames, count_of(kTenantSeparationNames)},
    {ObligationKey::PlacementGeographicDiversity, "placement.geographic_diversity", ValueKind::Enumerated,
     Strictness::Incomparable, 0, 4, kGeographicDiversityNames, count_of(kGeographicDiversityNames)},
    {ObligationKey::OperationsMonitoringIntervalSeconds, "operations.monitoring_interval_seconds",
     ValueKind::UnsignedInteger, Strictness::LowerIsStricter, 1, 86400, nullptr, 0},
    {ObligationKey::OperationsChangeControl, "operations.change_control", ValueKind::Enumerated,
     Strictness::Incomparable, 0, 3, kChangeControlNames, count_of(kChangeControlNames)},
    {ObligationKey::OperationsOnCallResponseSeconds, "operations.on_call_response_seconds",
     ValueKind::UnsignedInteger, Strictness::LowerIsStricter, 0, 86400, nullptr, 0},
    {ObligationKey::OperationsIncidentNotificationSeconds, "operations.incident_notification_seconds",
     ValueKind::UnsignedInteger, Strictness::LowerIsStricter, 0, 86400, nullptr, 0},
};

constexpr std::size_t kDescriptorCount = sizeof(kDescriptors) / sizeof(kDescriptors[0]);
static_assert(kDescriptorCount == kObligationKeyCount + 1, "the descriptor table must cover every obligation key");

bool is_declared_enumerator(const KeyDescriptor& descriptor, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < descriptor.enum_count; ++index) {
    if (descriptor.enum_names[index].value == value) {
      return true;
    }
  }
  return false;
}

}  // namespace

const char* to_string(Modality modality) noexcept {
  switch (modality) {
    case Modality::Unspecified:
      return "unspecified";
    case Modality::PermittedDegraded:
      return "permitted-degraded";
    case Modality::Preferred:
      return "preferred";
    case Modality::Required:
      return "required";
  }
  return "unknown";
}

Result<Modality> parse_modality(std::string_view text) {
  if (text == "unspecified") {
    return Result<Modality>::success(Modality::Unspecified);
  }
  if (text == "permitted-degraded") {
    return Result<Modality>::success(Modality::PermittedDegraded);
  }
  if (text == "preferred") {
    return Result<Modality>::success(Modality::Preferred);
  }
  if (text == "required") {
    return Result<Modality>::success(Modality::Required);
  }
  return Result<Modality>::failure(ErrorCode::InvalidEnum,
                                   "unknown modality '" + std::string(text) + "'", "modality");
}

const KeyDescriptor& describe(ObligationKey key) {
  const auto index = static_cast<std::size_t>(key);
  if (index >= kDescriptorCount) {
    return kDescriptors[0];
  }
  return kDescriptors[index];
}

const KeyDescriptor* find_key(std::string_view name) {
  for (std::size_t index = 1; index < kDescriptorCount; ++index) {
    if (name == kDescriptors[index].name) {
      return &kDescriptors[index];
    }
  }
  return nullptr;
}

const KeyDescriptor& key_at(std::size_t index) {
  if (index + 1 >= kDescriptorCount) {
    return kDescriptors[0];
  }
  return kDescriptors[index + 1];
}

bool is_valid_obligation_key(std::uint16_t raw) noexcept {
  return raw >= 1 && raw < kDescriptorCount;
}

Result<Obligation> Obligation::create(ObligationKey key, Modality modality, std::uint64_t value) {
  const auto raw = static_cast<std::uint16_t>(key);
  if (!is_valid_obligation_key(raw)) {
    return Result<Obligation>::failure(ErrorCode::InvalidEnum,
                                       "obligation key " + std::to_string(raw) + " is not a defined key", "obligation");
  }
  const KeyDescriptor& descriptor = describe(key);

  if (modality == Modality::Unspecified) {
    if (value != 0) {
      return Result<Obligation>::failure(ErrorCode::InvalidArgument,
                                         std::string("obligation '") + descriptor.name +
                                             "' is explicitly unspecified and therefore must carry the value 0",
                                         descriptor.name);
    }
    Obligation obligation;
    obligation.key_ = key;
    obligation.modality_ = modality;
    obligation.value_ = 0;
    return Result<Obligation>::success(obligation);
  }

  if (descriptor.kind == ValueKind::Enumerated) {
    if (!is_declared_enumerator(descriptor, value)) {
      return Result<Obligation>::failure(
          ErrorCode::InvalidEnum,
          std::string("value ") + std::to_string(value) + " is not a declared enumerator of '" + descriptor.name + "'",
          descriptor.name);
    }
    if (value == 0) {
      return Result<Obligation>::failure(
          ErrorCode::InvalidArgument,
          std::string("obligation '") + descriptor.name + "' cannot be asserted with the unspecified enumerator",
          descriptor.name);
    }
  } else {
    if (value < descriptor.min_value || value > descriptor.max_value) {
      return Result<Obligation>::failure(
          ErrorCode::OutOfRange,
          std::string("value ") + std::to_string(value) + " for '" + descriptor.name + "' is outside the accepted range [" +
              std::to_string(descriptor.min_value) + ", " + std::to_string(descriptor.max_value) + "]",
          descriptor.name);
    }
  }

  Obligation obligation;
  obligation.key_ = key;
  obligation.modality_ = modality;
  obligation.value_ = value;
  return Result<Obligation>::success(obligation);
}

std::string Obligation::value_text() const {
  const KeyDescriptor& descriptor = describe(key_);
  if (descriptor.kind == ValueKind::Enumerated) {
    for (std::size_t index = 0; index < descriptor.enum_count; ++index) {
      if (descriptor.enum_names[index].value == value_) {
        return descriptor.enum_names[index].name;
      }
    }
    return "invalid";
  }
  return std::to_string(value_);
}

MergeOrder compare_modality(Modality a, Modality b) noexcept {
  const auto left = static_cast<std::uint8_t>(a);
  const auto right = static_cast<std::uint8_t>(b);
  if (left == right) {
    return MergeOrder::Equal;
  }
  return left > right ? MergeOrder::LeftDominates : MergeOrder::RightDominates;
}

StrictnessComparison::Relation compare_strictness(ObligationKey key, std::uint64_t a, std::uint64_t b) noexcept {
  if (a == b) {
    return StrictnessComparison::Relation::Equal;
  }
  switch (describe(key).strictness) {
    case Strictness::HigherIsStricter:
      return a > b ? StrictnessComparison::Relation::AStricter : StrictnessComparison::Relation::BStricter;
    case Strictness::LowerIsStricter:
      return a < b ? StrictnessComparison::Relation::AStricter : StrictnessComparison::Relation::BStricter;
    case Strictness::Incomparable:
      return StrictnessComparison::Relation::Incomparable;
  }
  return StrictnessComparison::Relation::Incomparable;
}

VoidResult ObligationSet::add(const Obligation& obligation) {
  const auto found = items_.find(obligation.key());
  if (found != items_.end()) {
    const KeyDescriptor& descriptor = describe(obligation.key());
    return VoidResult::failure(ErrorCode::DuplicateObligation,
                               std::string("obligation '") + descriptor.name +
                                   "' is already declared in this revision; declare each key at most once",
                               descriptor.name);
  }
  items_.emplace(obligation.key(), obligation);
  return ok_result();
}

Result<Obligation> ObligationSet::get(ObligationKey key) const {
  const auto found = items_.find(key);
  if (found == items_.end()) {
    return Result<Obligation>::failure(ErrorCode::UnknownField,
                                       std::string("obligation '") + describe(key).name + "' is not declared",
                                       describe(key).name);
  }
  return Result<Obligation>::success(found->second);
}

}  // namespace scr
