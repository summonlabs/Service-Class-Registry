#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace scr {

// Closed error taxonomy. Values are part of the public contract: they are
// stable, documented in README.md, and never renumbered.
enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidArgument = 1,
  InvalidIdentifier = 2,
  InvalidEnum = 3,
  InvalidTextEncoding = 4,
  OutOfRange = 5,
  BoundExceeded = 6,
  DuplicateIdentity = 7,
  DuplicateObligation = 8,
  ContradictoryObligations = 9,
  MissingRequirement = 10,
  WeakenedRequirement = 11,
  CycleDetected = 12,
  ImmutableRevision = 13,
  LineageMismatch = 14,
  StaleTip = 15,
  UnknownClass = 16,
  UnknownRevision = 17,
  RetiredClass = 18,
  DigestMismatch = 19,
  UnsupportedVersion = 20,
  TruncatedInput = 21,
  CorruptStore = 22,
  RollbackDetected = 23,
  StoreBusy = 24,
  StoreNotInitialized = 25,
  AlreadyInitialized = 26,
  Overflow = 27,
  UnauthorizedTransition = 28,
  IoFailure = 29,
  UnknownField = 30,
  ParseError = 31,
  Internal = 32,
  // Advisory-only codes. These describe a merge or lifecycle decision that was
  // made deterministically and is surfaced for explainability; they never
  // appear as the code of a failed Status.
  MergedStricter = 33,
  DominatedByModality = 34,
  ExplicitUnspecifiedOverridden = 35,
  StrengthenedInheritance = 36,
  IdempotentReplay = 37,
  HistoricalRetiredParent = 38,
  GuardRepaired = 39,
  OrphanRecordsPresent = 40,
  StaleAuthority = 41,
};

const char* to_string(ErrorCode code) noexcept;

// Info: context. Advisory: the operation succeeded but something was merged,
// dominated, or otherwise worth surfacing. Refusal: the operation failed.
enum class Severity : std::uint8_t { Info = 0, Advisory = 1, Refusal = 2 };
const char* to_string(Severity severity) noexcept;

struct Diagnostic {
  ErrorCode code = ErrorCode::Ok;
  Severity severity = Severity::Info;
  std::string subject;
  std::string detail;
};

// A failed Status always carries at least one diagnostic whose severity is
// Refusal; diagnostics()[0] is the primary (first-precedence) failure and any
// remaining entries are suppressed secondary evidence that was observed before
// the decision was made.
class Status {
 public:
  Status() noexcept = default;

  static Status success() noexcept { return Status(); }

  static Status failure(ErrorCode code, std::string detail, std::string subject = {});

  static Status failure(Diagnostic primary, std::vector<Diagnostic> secondary = {});

  bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  explicit operator bool() const noexcept { return ok(); }

  ErrorCode code() const noexcept { return code_; }
  const std::string& subject() const noexcept { return subject_; }
  const std::string& detail() const noexcept { return detail_; }
  const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }

  void add_secondary(Diagnostic diagnostic);

  std::string to_string() const;

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string subject_;
  std::string detail_;
  std::vector<Diagnostic> diagnostics_;
};

}  // namespace scr
