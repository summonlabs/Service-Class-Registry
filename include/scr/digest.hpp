#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "scr/result.hpp"

namespace scr {

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4). Implemented first-party so that content binding and
// store integrity never depend on an external cryptography library.
// ---------------------------------------------------------------------------

class Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;
  static constexpr std::size_t kBlockBytes = 64;

  Sha256() noexcept;

  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void update(const void* data, std::size_t length) noexcept;
  void update(std::string_view text) noexcept;

  // Finalizes the digest. Must be called exactly once; the object is reset to
  // the initial state afterwards so that it can digest another message.
  std::array<std::uint8_t, kDigestBytes> finish() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, kBlockBytes> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
};

std::array<std::uint8_t, Sha256::kDigestBytes> sha256(const void* data, std::size_t length) noexcept;
std::array<std::uint8_t, Sha256::kDigestBytes> sha256(std::string_view text) noexcept;

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320). Used only as cheap
// framing protection; SHA-256 provides the binding strength.
std::uint32_t crc32(const void* data, std::size_t length) noexcept;
std::uint32_t crc32(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Digest: a 32-byte SHA-256 value with a strict canonical text form.
// A default-constructed Digest is the null digest (all zero bytes), which is
// never a valid content binding; is_null() distinguishes "explicitly unbound"
// from "a real digest".
// ---------------------------------------------------------------------------

class Digest {
 public:
  static constexpr std::size_t kBytes = Sha256::kDigestBytes;
  static constexpr std::size_t kHexChars = kBytes * 2;

  Digest() noexcept = default;

  static Digest null() noexcept { return Digest(); }

  static Digest from_bytes(const std::array<std::uint8_t, kBytes>& bytes) noexcept;

  static Digest of(const void* data, std::size_t length) noexcept;
  static Digest of(std::string_view text) noexcept;

  // Strict parse: exactly 64 hexadecimal characters, no whitespace, no prefix.
  // Upper case is accepted and normalized to lower case; anything else fails.
  static Result<Digest> parse(std::string_view hex);

  const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }

  bool is_null() const noexcept;

  std::string hex() const;
  // First 12 hex characters, for human-facing output only. Never a binding.
  std::string short_hex() const;

  friend bool operator==(const Digest& a, const Digest& b) noexcept { return a.bytes_ == b.bytes_; }
  friend bool operator!=(const Digest& a, const Digest& b) noexcept { return !(a == b); }
  friend bool operator<(const Digest& a, const Digest& b) noexcept { return a.bytes_ < b.bytes_; }

 private:
  std::array<std::uint8_t, kBytes> bytes_{};
};

}  // namespace scr
