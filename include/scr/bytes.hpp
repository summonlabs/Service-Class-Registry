#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "scr/result.hpp"

namespace scr {

// Bounded little-endian byte writer/reader used by every canonical encoding.
// The reader never reads past the declared payload, never allocates from an
// unvalidated length, and reports trailing bytes as an error.

class ByteWriter {
 public:
  void put_u8(std::uint8_t value);
  void put_u16(std::uint16_t value);
  void put_u32(std::uint32_t value);
  void put_u64(std::uint64_t value);
  void put_i64(std::int64_t value);
  void put_bytes(const void* data, std::size_t length);
  // Length-prefixed string (u32 byte length followed by raw bytes).
  void put_string(std::string_view text);

  const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
  std::vector<std::uint8_t> take() noexcept { return std::move(bytes_); }
  std::size_t size() const noexcept { return bytes_.size(); }

 private:
  std::vector<std::uint8_t> bytes_;
};

class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

  std::size_t remaining() const noexcept { return size_ - offset_; }
  std::size_t offset() const noexcept { return offset_; }

  Result<std::uint8_t> take_u8();
  Result<std::uint16_t> take_u16();
  Result<std::uint32_t> take_u32();
  Result<std::uint64_t> take_u64();
  Result<std::int64_t> take_i64();
  Result<std::vector<std::uint8_t>> take_bytes(std::size_t length);
  // Length-prefixed string with an explicit bound on the accepted length.
  Result<std::string> take_string(std::size_t max_bytes);
  // Reads a length-prefixed string and validates it as UTF-8 text without
  // control characters before returning it.
  Result<std::string> take_text(std::size_t max_bytes);

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t offset_ = 0;
};

}  // namespace scr
