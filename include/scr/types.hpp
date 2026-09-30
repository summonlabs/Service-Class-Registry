#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "scr/digest.hpp"
#include "scr/result.hpp"

namespace scr {

// ---------------------------------------------------------------------------
// Checked integer arithmetic. No generation, revision, sequence, size, or
// distance wraps silently anywhere in this library.
// ---------------------------------------------------------------------------

Result<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b);
Result<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b);

// ---------------------------------------------------------------------------
// Text validation
// ---------------------------------------------------------------------------

// Strict UTF-8: rejects overlong encodings, surrogate code points, values above
// U+10FFFF, truncated sequences, and embedded NUL bytes.
bool is_valid_utf8(std::string_view text) noexcept;

// True when the text contains any C0 control (0x00-0x1F) or DEL (0x7F).
bool has_control_characters(std::string_view text) noexcept;

// Validated, bounded UTF-8 text. All construction paths validate.
class BoundedText {
 public:
  BoundedText() = default;

  static Result<BoundedText> create(std::string value, std::size_t max_bytes);

  const std::string& value() const noexcept { return value_; }
  bool empty() const noexcept { return value_.empty(); }

  friend bool operator==(const BoundedText& a, const BoundedText& b) noexcept { return a.value_ == b.value_; }
  friend bool operator!=(const BoundedText& a, const BoundedText& b) noexcept { return !(a == b); }
  friend bool operator<(const BoundedText& a, const BoundedText& b) noexcept { return a.value_ < b.value_; }

 private:
  std::string value_;
};

// Metadata field bounds (bytes, not code points). Every field is optional;
// absence is distinct from an empty string only where the field is required.
inline constexpr std::size_t kTitleMaxBytes = 200;
inline constexpr std::size_t kSummaryMaxBytes = 1024;
inline constexpr std::size_t kOwnerMaxBytes = 200;
inline constexpr std::size_t kDocumentationMaxBytes = 4096;
inline constexpr std::size_t kRetireReasonMaxBytes = 512;
inline constexpr std::size_t kReferenceNoteMaxBytes = 256;

// Canonical class identity: "namespace/name". Both tokens are lower-case ASCII
// tokens; the whole identity is at most 128 bytes. Case is significant only in
// the sense that upper case is refused outright, so the text form is canonical.
inline constexpr std::size_t kClassIdMaxBytes = 128;
inline constexpr std::size_t kClassIdTokenMaxBytes = 63;

bool is_valid_identifier_token(std::string_view token) noexcept;

// Slash-separated identifier path used for external reference targets, e.g.
// "facility.policy/tenant-floor". At most 8 segments, 200 bytes total.
bool is_valid_identifier_path(std::string_view text, std::size_t max_segments) noexcept;
inline constexpr std::size_t kReferenceTargetMaxBytes = 200;
inline constexpr std::size_t kReferenceTargetMaxSegments = 8;

class ClassId {
 public:
  ClassId() = default;

  static Result<ClassId> parse(std::string_view text);
  static Result<ClassId> compose(std::string_view name_space, std::string_view name);

  bool empty() const noexcept { return text_.empty(); }
  std::string_view name_space() const noexcept;
  std::string_view name() const noexcept;
  const std::string& str() const noexcept { return text_; }

  friend bool operator==(const ClassId& a, const ClassId& b) noexcept { return a.text_ == b.text_; }
  friend bool operator!=(const ClassId& a, const ClassId& b) noexcept { return !(a == b); }
  friend bool operator<(const ClassId& a, const ClassId& b) noexcept { return a.text_ < b.text_; }

 private:
  std::string text_;
};

// Authority identity: the component that owns the authoritative class
// definitions. Validated token, never derived from a host name or user name.
class AuthorityId {
 public:
  AuthorityId() = default;

  static Result<AuthorityId> parse(std::string_view text);

  bool empty() const noexcept { return text_.empty(); }
  const std::string& str() const noexcept { return text_; }

  friend bool operator==(const AuthorityId& a, const AuthorityId& b) noexcept { return a.text_ == b.text_; }
  friend bool operator!=(const AuthorityId& a, const AuthorityId& b) noexcept { return !(a == b); }

 private:
  std::string text_;
};

inline constexpr std::size_t kAuthorityIdMaxBytes = 64;

// Monotonic counters. Zero is the "unset" sentinel of the strong type; content
// validation requires generation >= 1 and revision >= 1 wherever they identify
// a published revision.
class Generation {
 public:
  constexpr Generation() noexcept = default;
  static constexpr Generation from(std::uint64_t value) noexcept { return Generation(value); }

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == 0; }

  // Returns Overflow instead of wrapping at UINT64_MAX.
  Result<Generation> next() const;

  friend constexpr bool operator==(Generation a, Generation b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Generation a, Generation b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Generation a, Generation b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator<=(Generation a, Generation b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>(Generation a, Generation b) noexcept { return b < a; }
  friend constexpr bool operator>=(Generation a, Generation b) noexcept { return b <= a; }

 private:
  explicit constexpr Generation(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_ = 0;
};

class Revision {
 public:
  constexpr Revision() noexcept = default;
  static constexpr Revision from(std::uint32_t value) noexcept { return Revision(value); }

  constexpr std::uint32_t value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == 0; }

  // Returns Overflow instead of wrapping at UINT32_MAX.
  Result<Revision> next() const;

  friend constexpr bool operator==(Revision a, Revision b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Revision a, Revision b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Revision a, Revision b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator<=(Revision a, Revision b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>(Revision a, Revision b) noexcept { return b < a; }
  friend constexpr bool operator>=(Revision a, Revision b) noexcept { return b <= a; }

 private:
  explicit constexpr Revision(std::uint32_t value) noexcept : value_(value) {}
  std::uint32_t value_ = 0;
};

// Store control epoch. A new epoch invalidates live authority held by an older
// incarnation; it is stored in the manifest and fenced by the guard file.
class Epoch {
 public:
  constexpr Epoch() noexcept = default;
  static constexpr Epoch from(std::uint64_t value) noexcept { return Epoch(value); }

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == 0; }

  friend constexpr bool operator==(Epoch a, Epoch b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Epoch a, Epoch b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Epoch a, Epoch b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator<=(Epoch a, Epoch b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>(Epoch a, Epoch b) noexcept { return b < a; }
  friend constexpr bool operator>=(Epoch a, Epoch b) noexcept { return b <= a; }

 private:
  explicit constexpr Epoch(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_ = 0;
};

// Manifest sequence: strictly increasing commit counter. The guard file records
// the highest sequence that was ever published, which is what makes a restored
// older manifest detectable.
class Sequence {
 public:
  constexpr Sequence() noexcept = default;
  static constexpr Sequence from(std::uint64_t value) noexcept { return Sequence(value); }

  constexpr std::uint64_t value() const noexcept { return value_; }

  Result<Sequence> next() const;

  friend constexpr bool operator==(Sequence a, Sequence b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Sequence a, Sequence b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Sequence a, Sequence b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator>(Sequence a, Sequence b) noexcept { return b < a; }

 private:
  explicit constexpr Sequence(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_ = 0;
};

// Wall-clock timestamp. Optional by construction: "unset" never collapses to
// zero, and a timestamp is never authority. The upper bound is
// 253402300799 (9999-12-31T23:59:59Z).
class Timestamp {
 public:
  static constexpr std::int64_t kMaxUnixSeconds = 253402300799;

  Timestamp() = default;

  static Timestamp unset() noexcept { return Timestamp(); }
  static Result<Timestamp> from_unix_seconds(std::int64_t seconds);

  bool has_value() const noexcept { return seconds_.has_value(); }
  std::int64_t unix_seconds() const noexcept { return seconds_.value_or(0); }

  // "unset" when no value is present, otherwise "YYYY-MM-DDTHH:MM:SSZ".
  std::string to_iso8601() const;

  friend bool operator==(const Timestamp& a, const Timestamp& b) noexcept { return a.seconds_ == b.seconds_; }
  friend bool operator!=(const Timestamp& a, const Timestamp& b) noexcept { return !(a == b); }

 private:
  std::optional<std::int64_t> seconds_;
};

// Exact revision identity: generation + revision + content digest. Every
// binding recorded by a consumer is one of these; a binding that omits the
// digest cannot be verified and is refused at the API boundary.
struct RevisionRef {
  Generation generation;
  Revision revision;
  Digest digest;

  // "<generation>.<revision>@<64 hex chars>"
  std::string to_string() const;
  // "<generation>.<revision>@<12 hex chars>#<short digest>" for humans only.
  std::string to_display_string() const;

  friend bool operator==(const RevisionRef& a, const RevisionRef& b) noexcept {
    return a.generation == b.generation && a.revision == b.revision && a.digest == b.digest;
  }
  friend bool operator!=(const RevisionRef& a, const RevisionRef& b) noexcept { return !(a == b); }
  friend bool operator<(const RevisionRef& a, const RevisionRef& b) noexcept {
    if (a.generation != b.generation) {
      return a.generation < b.generation;
    }
    if (a.revision != b.revision) {
      return a.revision < b.revision;
    }
    return a.digest < b.digest;
  }
};

// A class identity plus the exact revision that a consumer decision is bound to.
struct ClassBinding {
  ClassId class_id;
  RevisionRef revision;

  std::string to_string() const;

  friend bool operator==(const ClassBinding& a, const ClassBinding& b) noexcept {
    return a.class_id == b.class_id && a.revision == b.revision;
  }
  friend bool operator!=(const ClassBinding& a, const ClassBinding& b) noexcept { return !(a == b); }
};

}  // namespace scr
