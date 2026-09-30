#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scr/digest.hpp"
#include "scr/revision.hpp"
#include "scr/result.hpp"

namespace scr {

// Canonical frame format (SCRF), version 1, little endian:
//
//   [ 0.. 3] magic           4 bytes, the ASCII bytes S C R F
//   [ 4.. 5] format_version  u16
//   [ 6]     kind            u8
//   [ 7]     flags           u8, reserved, must be zero
//   [ 8..11] payload_length  u32
//   [12..  ] payload         payload_length bytes
//   [16+len] crc32           u32 over bytes [0, 12 + payload_length)
//
// The total byte length must be exactly kFrameOverheadBytes + payload_length.
// Shorter or longer input is refused: declared lengths are never trusted and
// trailing garbage is never ignored. The digest of a frame is the SHA-256 of
// the complete frame, which is why the frame, not just the payload, is the unit
// that gets bound.

enum class FrameKind : std::uint8_t {
  RevisionContent = 1,
  StoreManifest = 2,
  StoreGuard = 3,
};

const char* to_string(FrameKind kind) noexcept;

inline constexpr std::size_t kFrameHeaderBytes = 12;
inline constexpr std::size_t kFrameTrailerBytes = 4;
inline constexpr std::size_t kFrameOverheadBytes = kFrameHeaderBytes + kFrameTrailerBytes;

inline constexpr std::uint32_t kMaxRevisionPayloadBytes = 1048576u;
inline constexpr std::uint32_t kMaxManifestPayloadBytes = 67108864u;
inline constexpr std::uint32_t kMaxGuardPayloadBytes = 48;

struct DecodedFrame {
  FrameKind kind = FrameKind::RevisionContent;
  std::uint16_t format_version = 0;
  std::vector<std::uint8_t> payload;
  Digest frame_digest;
};

// Encodes a frame with the current format version. Fails with BoundExceeded
// when the payload exceeds max_payload.
Result<std::vector<std::uint8_t>> encode_frame(FrameKind kind, const std::vector<std::uint8_t>& payload,
                                               std::uint32_t max_payload);

// Validates magic, version, kind, flags, declared length, exact total length,
// and CRC before returning the payload. max_payload bounds the accepted
// declared length; the caller-supplied buffer is never over-read.
Result<DecodedFrame> decode_frame(const std::uint8_t* data, std::size_t size, FrameKind expected_kind,
                                  std::uint32_t max_payload);

// Revision content payload codec. Round trips are exact and bounded.
Result<std::vector<std::uint8_t>> encode_revision_payload(const ClassRevision& revision);
Result<ClassRevision> decode_revision_payload(const std::vector<std::uint8_t>& payload);

// Full frame codec for revision content.
Result<std::vector<std::uint8_t>> encode_revision_frame(const ClassRevision& revision);
Result<ClassRevision> decode_revision_frame(const std::uint8_t* data, std::size_t size);
Result<Digest> revision_frame_digest(const ClassRevision& revision);

// Payload codec for the store guard (fixed 48-byte payload).
struct GuardRecord {
  Sequence sequence;
  Digest manifest_digest;
  Epoch epoch;
};
Result<std::vector<std::uint8_t>> encode_guard_payload(const GuardRecord& guard);
Result<GuardRecord> decode_guard_payload(const std::vector<std::uint8_t>& payload);

}  // namespace scr
