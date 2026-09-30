#include "scr/status.hpp"

#include <array>

namespace scr {
namespace {

constexpr std::array<const char*, 42> kErrorNames = {
    "Ok",
    "InvalidArgument",
    "InvalidIdentifier",
    "InvalidEnum",
    "InvalidTextEncoding",
    "OutOfRange",
    "BoundExceeded",
    "DuplicateIdentity",
    "DuplicateObligation",
    "ContradictoryObligations",
    "MissingRequirement",
    "WeakenedRequirement",
    "CycleDetected",
    "ImmutableRevision",
    "LineageMismatch",
    "StaleTip",
    "UnknownClass",
    "UnknownRevision",
    "RetiredClass",
    "DigestMismatch",
    "UnsupportedVersion",
    "TruncatedInput",
    "CorruptStore",
    "RollbackDetected",
    "StoreBusy",
    "StoreNotInitialized",
    "AlreadyInitialized",
    "Overflow",
    "UnauthorizedTransition",
    "IoFailure",
    "UnknownField",
    "ParseError",
    "Internal",
    "MergedStricter",
    "DominatedByModality",
    "ExplicitUnspecifiedOverridden",
    "StrengthenedInheritance",
    "IdempotentReplay",
    "HistoricalRetiredParent",
    "GuardRepaired",
    "OrphanRecordsPresent",
    "StaleAuthority",
};

}  // namespace

const char* to_string(ErrorCode code) noexcept {
  const auto index = static_cast<std::size_t>(code);
  if (index >= kErrorNames.size()) {
    return "UnknownErrorCode";
  }
  return kErrorNames[index];
}

const char* to_string(Severity severity) noexcept {
  switch (severity) {
    case Severity::Info:
      return "info";
    case Severity::Advisory:
      return "advisory";
    case Severity::Refusal:
      return "refusal";
  }
  return "unknown";
}

Status Status::failure(ErrorCode code, std::string detail, std::string subject) {
  Diagnostic diagnostic;
  diagnostic.code = code;
  diagnostic.severity = Severity::Refusal;
  diagnostic.subject = std::move(subject);
  diagnostic.detail = std::move(detail);
  return failure(std::move(diagnostic));
}

Status Status::failure(Diagnostic primary, std::vector<Diagnostic> secondary) {
  Status status;
  status.code_ = primary.code;
  status.subject_ = primary.subject;
  status.detail_ = primary.detail;
  if (status.code_ == ErrorCode::Ok) {
    // A caller that builds a failure from an Ok code is a programming error;
    // surface it as Internal rather than reporting success.
    status.code_ = ErrorCode::Internal;
    status.detail_ = "failure status constructed with ErrorCode::Ok";
    primary.code = ErrorCode::Internal;
    primary.severity = Severity::Refusal;
  }
  primary.severity = Severity::Refusal;
  status.diagnostics_.reserve(1 + secondary.size());
  status.diagnostics_.push_back(std::move(primary));
  for (auto& diagnostic : secondary) {
    status.diagnostics_.push_back(std::move(diagnostic));
  }
  return status;
}

void Status::add_secondary(Diagnostic diagnostic) {
  if (ok()) {
    return;
  }
  diagnostics_.push_back(std::move(diagnostic));
}

std::string Status::to_string() const {
  if (ok()) {
    return "ok";
  }
  std::string text = scr::to_string(code_);
  if (!subject_.empty()) {
    text += " [";
    text += subject_;
    text += "]";
  }
  if (!detail_.empty()) {
    text += ": ";
    text += detail_;
  }
  return text;
}

}  // namespace scr
