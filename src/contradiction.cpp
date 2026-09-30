#include "scr/contradiction.hpp"

#include <array>

namespace scr {
namespace {

constexpr std::uint64_t kSecondsPerYear = 31536000;
constexpr std::uint64_t kPpmScale = 1000000;

struct RuleInfo {
  const char* id;
  const char* rationale;
};

constexpr std::array<RuleInfo, 17> kRules = {{
    {"R-01", "rolling, online, or concurrent-maintainable maintenance requires a non-zero concurrent fault tolerance"},
    {"R-02", "concurrent-maintainable maintenance requires a declared redundancy topology other than none"},
    {"R-03", "the tolerated concurrent fault count must be smaller than the number of independent fault domains"},
    {"R-04", "redundancy topology none permits at most one independent fault domain"},
    {"R-05", "manual or automatic failover requires a maximum failover time"},
    {"R-06", "failover mode none cannot carry a non-zero maximum failover time"},
    {"R-07", "the recovery time objective cannot exceed the annual downtime allowance"},
    {"R-08", "the declared annual downtime allowance cannot exceed what the availability target permits"},
    {"R-09", "power transfer requires at least two feeds to transfer between"},
    {"R-10", "independent power paths require at least two feeds"},
    {"R-11", "manual or automatic power transfer requires a maximum transfer time"},
    {"R-12", "transfer mode none cannot carry a non-zero maximum transfer time"},
    {"R-13", "a non-zero independent cooling path count requires a cooling mode other than none"},
    {"R-14", "more than one independent cooling path requires redundant mechanical cooling"},
    {"R-15", "physical tenant separation requires at least two isolation domains"},
    {"R-16", "geographic diversity requires at least two isolation domains"},
    {"R-17", "hot or continuous restore requires a declared recovery point objective"},
}};

// A participating obligation is one that is actually asserted: explicitly
// unspecified obligations are preserved but never drive a rule.
bool participates(const ObligationSet& set, ObligationKey key, const Obligation** out) noexcept {
  const auto found = set.items().find(key);
  if (found == set.items().end()) {
    return false;
  }
  if (found->second.modality() == Modality::Unspecified) {
    return false;
  }
  if (out != nullptr) {
    *out = &found->second;
  }
  return true;
}

// Refusal when every participating obligation is Required, advisory otherwise.
Severity severity_of(const std::vector<const Obligation*>& participants) noexcept {
  for (const Obligation* obligation : participants) {
    if (obligation->modality() != Modality::Required) {
      return Severity::Advisory;
    }
  }
  return Severity::Refusal;
}

std::string value_of(const std::string& key_name, const Obligation& obligation) {
  return key_name + "=" + obligation.value_text();
}

void add(std::vector<RuleViolation>& violations, std::size_t rule_index, ErrorCode code,
         std::vector<const Obligation*> participants, std::vector<ObligationKey> keys, std::string detail) {
  RuleViolation violation;
  violation.rule = kRules[rule_index].id;
  violation.code = code == ErrorCode::Ok ? ErrorCode::ContradictoryObligations : code;
  violation.severity = severity_of(participants);
  violation.keys = std::move(keys);
  violation.detail = std::move(detail);
  violations.push_back(std::move(violation));
}

}  // namespace

bool ContradictionReport::refused() const noexcept {
  for (const RuleViolation& violation : violations) {
    if (violation.severity == Severity::Refusal) {
      return true;
    }
  }
  return false;
}

std::size_t ContradictionReport::refusal_count() const noexcept {
  std::size_t count = 0;
  for (const RuleViolation& violation : violations) {
    if (violation.severity == Severity::Refusal) {
      ++count;
    }
  }
  return count;
}

std::size_t ContradictionReport::advisory_count() const noexcept {
  return violations.size() - refusal_count();
}

Result<RuleViolation> ContradictionReport::primary_refusal() const {
  for (const RuleViolation& violation : violations) {
    if (violation.severity == Severity::Refusal) {
      return Result<RuleViolation>::success(violation);
    }
  }
  return Result<RuleViolation>::failure(ErrorCode::InvalidArgument, "no rule was refused", "rules");
}

ContradictionReport evaluate_rules(const ObligationSet& effective) {
  ContradictionReport report;
  const Obligation* maintenance_mode = nullptr;
  const Obligation* tolerance = nullptr;
  const Obligation* domains = nullptr;
  const Obligation* topology = nullptr;
  const Obligation* failover_mode = nullptr;
  const Obligation* failover_seconds = nullptr;
  const Obligation* rto = nullptr;
  const Obligation* annual_downtime = nullptr;
  const Obligation* target_ppm = nullptr;
  const Obligation* transfer_mode = nullptr;
  const Obligation* feed_count = nullptr;
  const Obligation* path_independence = nullptr;
  const Obligation* transfer_seconds = nullptr;
  const Obligation* cooling_mode = nullptr;
  const Obligation* cooling_paths = nullptr;
  const Obligation* tenant_separation = nullptr;
  const Obligation* isolation_domains = nullptr;
  const Obligation* geographic_diversity = nullptr;
  const Obligation* restore_mode = nullptr;
  const Obligation* rpo = nullptr;

  const bool has_maintenance_mode = participates(effective, ObligationKey::MaintenanceMode, &maintenance_mode);
  const bool has_tolerance =
      participates(effective, ObligationKey::RedundancyConcurrentFaultTolerance, &tolerance);
  const bool has_domains =
      participates(effective, ObligationKey::RedundancyMinimumIndependentFaultDomains, &domains);
  const bool has_topology = participates(effective, ObligationKey::RedundancyTopology, &topology);
  const bool has_failover_mode = participates(effective, ObligationKey::RedundancyFailoverMode, &failover_mode);
  const bool has_failover_seconds =
      participates(effective, ObligationKey::RedundancyMaximumFailoverSeconds, &failover_seconds);
  const bool has_rto = participates(effective, ObligationKey::RecoveryTimeObjectiveSeconds, &rto);
  const bool has_annual_downtime =
      participates(effective, ObligationKey::AvailabilityMaxAnnualDowntimeSeconds, &annual_downtime);
  const bool has_target_ppm = participates(effective, ObligationKey::AvailabilityTargetPpm, &target_ppm);
  const bool has_transfer_mode = participates(effective, ObligationKey::PowerTransferMode, &transfer_mode);
  const bool has_feed_count = participates(effective, ObligationKey::PowerFeedCount, &feed_count);
  const bool has_path_independence =
      participates(effective, ObligationKey::PowerPathIndependence, &path_independence);
  const bool has_transfer_seconds =
      participates(effective, ObligationKey::PowerMaximumTransferSeconds, &transfer_seconds);
  const bool has_cooling_mode = participates(effective, ObligationKey::CoolingMode, &cooling_mode);
  const bool has_cooling_paths = participates(effective, ObligationKey::CoolingIndependentPaths, &cooling_paths);
  const bool has_tenant_separation =
      participates(effective, ObligationKey::PlacementTenantSeparation, &tenant_separation);
  const bool has_isolation_domains =
      participates(effective, ObligationKey::PlacementMinimumIsolationDomains, &isolation_domains);
  const bool has_geographic_diversity =
      participates(effective, ObligationKey::PlacementGeographicDiversity, &geographic_diversity);
  const bool has_restore_mode = participates(effective, ObligationKey::RecoveryRestoreMode, &restore_mode);
  const bool has_rpo = participates(effective, ObligationKey::RecoveryPointObjectiveSeconds, &rpo);

  const auto is_mode = [](const Obligation* obligation, MaintenanceMode mode) noexcept {
    return obligation != nullptr && obligation->value() == static_cast<std::uint64_t>(mode);
  };
  const auto is_failover = [](const Obligation* obligation, FailoverMode mode) noexcept {
    return obligation != nullptr && obligation->value() == static_cast<std::uint64_t>(mode);
  };
  const auto is_transfer = [](const Obligation* obligation, TransferMode mode) noexcept {
    return obligation != nullptr && obligation->value() == static_cast<std::uint64_t>(mode);
  };
  const auto is_topology = [](const Obligation* obligation, RedundancyTopology topology_value) noexcept {
    return obligation != nullptr && obligation->value() == static_cast<std::uint64_t>(topology_value);
  };

  // R-01
  if (has_maintenance_mode &&
      (is_mode(maintenance_mode, MaintenanceMode::Rolling) || is_mode(maintenance_mode, MaintenanceMode::Online) ||
       is_mode(maintenance_mode, MaintenanceMode::ConcurrentMaintainable)) &&
      has_tolerance && tolerance->value() == 0) {
    add(report.violations, 0, ErrorCode::ContradictoryObligations, {maintenance_mode, tolerance},
        {ObligationKey::MaintenanceMode, ObligationKey::RedundancyConcurrentFaultTolerance},
        value_of("maintenance.mode", *maintenance_mode) +
            " requires a non-zero redundancy.concurrent_fault_tolerance, but " + value_of("redundancy.concurrent_fault_tolerance", *tolerance));
  }

  // R-02
  if (has_maintenance_mode && is_mode(maintenance_mode, MaintenanceMode::ConcurrentMaintainable)) {
    const bool topology_blocks = !has_topology || is_topology(topology, RedundancyTopology::None);
    if (topology_blocks) {
      std::vector<const Obligation*> participants{maintenance_mode};
      if (has_topology) {
        participants.push_back(topology);
      }
      add(report.violations, 1, ErrorCode::MissingRequirement, participants,
          {ObligationKey::MaintenanceMode, ObligationKey::RedundancyTopology},
          has_topology ? value_of("redundancy.topology", *topology) + " cannot support concurrent-maintainable maintenance"
                       : std::string("concurrent-maintainable maintenance requires a declared redundancy.topology"));
    }
  }

  // R-03
  if (has_tolerance && tolerance->value() >= 1 && has_domains && domains->value() <= tolerance->value()) {
    add(report.violations, 2, ErrorCode::ContradictoryObligations, {tolerance, domains},
        {ObligationKey::RedundancyConcurrentFaultTolerance,
         ObligationKey::RedundancyMinimumIndependentFaultDomains},
        value_of("redundancy.concurrent_fault_tolerance", *tolerance) + " needs at least " +
            std::to_string(tolerance->value() + 1) + " independent fault domains, but " +
            value_of("redundancy.minimum_independent_fault_domains", *domains));
  }

  // R-04
  if (has_topology && is_topology(topology, RedundancyTopology::None) && has_domains && domains->value() > 1) {
    add(report.violations, 3, ErrorCode::ContradictoryObligations, {topology, domains},
        {ObligationKey::RedundancyTopology, ObligationKey::RedundancyMinimumIndependentFaultDomains},
        value_of("redundancy.topology", *topology) + " permits at most one independent fault domain, but " +
            value_of("redundancy.minimum_independent_fault_domains", *domains));
  }

  // R-05
  if (has_failover_mode &&
      (is_failover(failover_mode, FailoverMode::Manual) || is_failover(failover_mode, FailoverMode::Automatic)) &&
      !has_failover_seconds) {
    add(report.violations, 4, ErrorCode::MissingRequirement, {failover_mode},
        {ObligationKey::RedundancyFailoverMode, ObligationKey::RedundancyMaximumFailoverSeconds},
        value_of("redundancy.failover_mode", *failover_mode) +
            " requires redundancy.maximum_failover_seconds");
  }

  // R-06
  if (has_failover_mode && is_failover(failover_mode, FailoverMode::None) && has_failover_seconds &&
      failover_seconds->value() > 0) {
    add(report.violations, 5, ErrorCode::ContradictoryObligations, {failover_mode, failover_seconds},
        {ObligationKey::RedundancyFailoverMode, ObligationKey::RedundancyMaximumFailoverSeconds},
        value_of("redundancy.failover_mode", *failover_mode) + " cannot carry " +
            value_of("redundancy.maximum_failover_seconds", *failover_seconds));
  }

  // R-07
  if (has_rto && has_annual_downtime && rto->value() > annual_downtime->value()) {
    add(report.violations, 6, ErrorCode::ContradictoryObligations, {rto, annual_downtime},
        {ObligationKey::RecoveryTimeObjectiveSeconds,
         ObligationKey::AvailabilityMaxAnnualDowntimeSeconds},
        value_of("recovery.time_objective_seconds", *rto) + " exceeds the annual downtime allowance " +
            value_of("availability.max_annual_downtime_seconds", *annual_downtime));
  }

  // R-08
  if (has_target_ppm && has_annual_downtime) {
    const std::uint64_t permitted = (kSecondsPerYear * (kPpmScale - target_ppm->value())) / kPpmScale;
    if (annual_downtime->value() > permitted) {
      add(report.violations, 7, ErrorCode::ContradictoryObligations, {target_ppm, annual_downtime},
          {ObligationKey::AvailabilityTargetPpm, ObligationKey::AvailabilityMaxAnnualDowntimeSeconds},
          value_of("availability.target_ppm", *target_ppm) + " permits at most " + std::to_string(permitted) +
              " seconds of downtime per year, but " +
              value_of("availability.max_annual_downtime_seconds", *annual_downtime) + " is declared");
    }
  }

  // R-09
  if (has_transfer_mode &&
      (is_transfer(transfer_mode, TransferMode::Manual) || is_transfer(transfer_mode, TransferMode::Automatic)) &&
      (!has_feed_count || feed_count->value() < 2)) {
    std::vector<const Obligation*> participants{transfer_mode};
    if (has_feed_count) {
      participants.push_back(feed_count);
    }
    add(report.violations, 8, ErrorCode::ContradictoryObligations, participants,
        {ObligationKey::PowerTransferMode, ObligationKey::PowerFeedCount},
        value_of("power.transfer_mode", *transfer_mode) + " requires power.feed_count of at least 2");
  }

  // R-10
  if (has_path_independence && path_independence->value() >= static_cast<std::uint64_t>(PathIndependence::DualIndependent) &&
      (!has_feed_count || feed_count->value() < 2)) {
    std::vector<const Obligation*> participants{path_independence};
    if (has_feed_count) {
      participants.push_back(feed_count);
    }
    add(report.violations, 9, ErrorCode::ContradictoryObligations, participants,
        {ObligationKey::PowerPathIndependence, ObligationKey::PowerFeedCount},
        value_of("power.path_independence", *path_independence) + " requires power.feed_count of at least 2");
  }

  // R-11
  if (has_transfer_mode &&
      (is_transfer(transfer_mode, TransferMode::Manual) || is_transfer(transfer_mode, TransferMode::Automatic)) &&
      !has_transfer_seconds) {
    add(report.violations, 10, ErrorCode::MissingRequirement, {transfer_mode},
        {ObligationKey::PowerTransferMode, ObligationKey::PowerMaximumTransferSeconds},
        value_of("power.transfer_mode", *transfer_mode) + " requires power.maximum_transfer_seconds");
  }

  // R-12
  if (has_transfer_mode && is_transfer(transfer_mode, TransferMode::None) && has_transfer_seconds &&
      transfer_seconds->value() > 0) {
    add(report.violations, 11, ErrorCode::ContradictoryObligations, {transfer_mode, transfer_seconds},
        {ObligationKey::PowerTransferMode, ObligationKey::PowerMaximumTransferSeconds},
        value_of("power.transfer_mode", *transfer_mode) + " cannot carry " +
            value_of("power.maximum_transfer_seconds", *transfer_seconds));
  }

  // R-13
  if (has_cooling_paths && cooling_paths->value() >= 1 &&
      (!has_cooling_mode || cooling_mode->value() == static_cast<std::uint64_t>(CoolingMode::None))) {
    std::vector<const Obligation*> participants{cooling_paths};
    if (has_cooling_mode) {
      participants.push_back(cooling_mode);
    }
    add(report.violations, 12, ErrorCode::ContradictoryObligations, participants,
        {ObligationKey::CoolingIndependentPaths, ObligationKey::CoolingMode},
        value_of("cooling.independent_paths", *cooling_paths) + " requires a cooling mode other than none");
  }

  // R-14
  if (has_cooling_paths && cooling_paths->value() >= 2 && has_cooling_mode &&
      cooling_mode->value() != static_cast<std::uint64_t>(CoolingMode::RedundantMechanical)) {
    add(report.violations, 13, ErrorCode::ContradictoryObligations, {cooling_paths, cooling_mode},
        {ObligationKey::CoolingIndependentPaths, ObligationKey::CoolingMode},
        value_of("cooling.independent_paths", *cooling_paths) + " requires cooling.mode=redundant-mechanical, but " +
            value_of("cooling.mode", *cooling_mode));
  }

  // R-15
  if (has_tenant_separation &&
      tenant_separation->value() == static_cast<std::uint64_t>(TenantSeparation::Physical) &&
      (!has_isolation_domains || isolation_domains->value() < 2)) {
    std::vector<const Obligation*> participants{tenant_separation};
    if (has_isolation_domains) {
      participants.push_back(isolation_domains);
    }
    add(report.violations, 14, ErrorCode::ContradictoryObligations, participants,
        {ObligationKey::PlacementTenantSeparation, ObligationKey::PlacementMinimumIsolationDomains},
        value_of("placement.tenant_separation", *tenant_separation) +
            " requires placement.minimum_isolation_domains of at least 2");
  }

  // R-16
  if (has_geographic_diversity &&
      geographic_diversity->value() >= static_cast<std::uint64_t>(GeographicDiversity::SameCampus) &&
      (!has_isolation_domains || isolation_domains->value() < 2)) {
    std::vector<const Obligation*> participants{geographic_diversity};
    if (has_isolation_domains) {
      participants.push_back(isolation_domains);
    }
    add(report.violations, 15, ErrorCode::ContradictoryObligations, participants,
        {ObligationKey::PlacementGeographicDiversity, ObligationKey::PlacementMinimumIsolationDomains},
        value_of("placement.geographic_diversity", *geographic_diversity) +
            " requires placement.minimum_isolation_domains of at least 2");
  }

  // R-17
  if (has_restore_mode &&
      (restore_mode->value() == static_cast<std::uint64_t>(RestoreMode::Hot) ||
       restore_mode->value() == static_cast<std::uint64_t>(RestoreMode::Continuous)) &&
      !has_rpo) {
    add(report.violations, 16, ErrorCode::MissingRequirement, {restore_mode},
        {ObligationKey::RecoveryRestoreMode, ObligationKey::RecoveryPointObjectiveSeconds},
        value_of("recovery.restore_mode", *restore_mode) + " requires recovery.point_objective_seconds");
  }

  return report;
}

std::size_t rule_count() noexcept {
  return kRules.size();
}

const char* rule_id_at(std::size_t index) noexcept {
  if (index >= kRules.size()) {
    return "";
  }
  return kRules[index].id;
}

const char* rule_rationale(const char* rule) noexcept {
  for (const RuleInfo& info : kRules) {
    if (std::string_view(info.id) == std::string_view(rule)) {
      return info.rationale;
    }
  }
  return "";
}

}  // namespace scr
