#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "scr/obligation.hpp"
#include "scr/status.hpp"

namespace scr {

// Cross-key consistency: the closed rule set that decides whether a set of
// effective obligations is self-consistent. Rules are evaluated in a fixed
// order; every violation that applies is reported, and the first one is the
// primary refusal. Absent keys never fire a rule except where the rule states a
// completeness requirement, and obligations whose modality is Unspecified never
// participate in a rule.
struct RuleViolation {
  const char* rule = "";
  ErrorCode code = ErrorCode::ContradictoryObligations;
  Severity severity = Severity::Refusal;
  std::vector<ObligationKey> keys;
  std::string detail;
};

struct ContradictionReport {
  std::vector<RuleViolation> violations;  // evaluation order: fixed rule order

  bool refused() const noexcept;
  std::size_t refusal_count() const noexcept;
  std::size_t advisory_count() const noexcept;

  // Primary refusal: the first refusal in rule order, or a failure status when
  // nothing was refused.
  Result<RuleViolation> primary_refusal() const;
};

ContradictionReport evaluate_rules(const ObligationSet& effective);

// Stable identifiers and one-line rationales for the closed rule set. Used by
// the explain API and by the documented rule table.
std::size_t rule_count() noexcept;
const char* rule_id_at(std::size_t index) noexcept;
const char* rule_rationale(const char* rule) noexcept;

}  // namespace scr
