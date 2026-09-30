#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "scr/registry.hpp"

namespace scr {

// ---------------------------------------------------------------------------
// Canonical text form (SCX), version 1
//
//   scr-class 1
//   class facility.core/dc-hall-a
//   generation 1
//   revision 3
//   lineage 1.2@<64 hex chars>            (or: lineage genesis)
//   parent facility.core/power-train @ 1.2@<64 hex chars>
//   obligation redundancy.topology required n+1
//   obligation availability.target_ppm required 99990000
//   reference policy-predicate facility.policy/tenant-floor @<64 hex|unbound> authority=scr-primary
//   text title "..."
//
// The header line is mandatory and must come first. Directives after it may
// appear in any order; obligations, references, and parents are canonicalized
// deterministically, so two files that declare the same semantics in different
// orders produce byte-identical canonical frames and identical digests.
//
// The parser is strict: unknown directives, unknown keys, unknown enumerators,
// duplicate directives, duplicate keys, malformed references, out-of-range
// integers, hostile control characters, and invalid UTF-8 are all refused with
// the line number and the reason.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxScxBytes = 1u << 20;
inline constexpr std::size_t kMaxScxLineBytes = 8192;

Result<ClassRevision> parse_scx(std::string_view text);
std::string emit_scx(const ClassRevision& revision);

// Lowercase hex of the canonical binary frame. This is the artifact a consumer
// can persist and later verify against the published digest.
std::string emit_frame_hex(const ClassRevision& revision);
Result<ClassRevision> parse_frame_hex(std::string_view hex);

}  // namespace scr
