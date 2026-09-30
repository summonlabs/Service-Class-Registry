#include "scr/bytes.hpp"

#include "scr/types.hpp"

namespace scr {

void ByteWriter::put_u8(std::uint8_t value) {
  bytes_.push_back(value);
}

void ByteWriter::put_u16(std::uint16_t value) {
  bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  bytes_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void ByteWriter::put_u32(std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    bytes_.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu));
  }
}

void ByteWriter::put_u64(std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    bytes_.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu));
  }
}

void ByteWriter::put_i64(std::int64_t value) {
  put_u64(static_cast<std::uint64_t>(value));
}

void ByteWriter::put_bytes(const void* data, std::size_t length) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  bytes_.insert(bytes_.end(), bytes, bytes + length);
}

void ByteWriter::put_string(std::string_view text) {
  put_u32(static_cast<std::uint32_t>(text.size()));
  put_bytes(text.data(), text.size());
}

Result<std::uint8_t> ByteReader::take_u8() {
  if (remaining() < 1) {
    return Result<std::uint8_t>::failure(ErrorCode::TruncatedInput, "input ended while reading a byte");
  }
  return Result<std::uint8_t>::success(data_[offset_++]);
}

Result<std::uint16_t> ByteReader::take_u16() {
  if (remaining() < 2) {
    return Result<std::uint16_t>::failure(ErrorCode::TruncatedInput, "input ended while reading a u16");
  }
  const std::uint32_t raw = static_cast<std::uint32_t>(data_[offset_]) |
                           (static_cast<std::uint32_t>(data_[offset_ + 1]) << 8);
  offset_ += 2;
  return Result<std::uint16_t>::success(static_cast<std::uint16_t>(raw));
}

Result<std::uint32_t> ByteReader::take_u32() {
  if (remaining() < 4) {
    return Result<std::uint32_t>::failure(ErrorCode::TruncatedInput, "input ended while reading a u32");
  }
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data_[offset_ + static_cast<std::size_t>(index)]) << (8 * index);
  }
  offset_ += 4;
  return Result<std::uint32_t>::success(value);
}

Result<std::uint64_t> ByteReader::take_u64() {
  if (remaining() < 8) {
    return Result<std::uint64_t>::failure(ErrorCode::TruncatedInput, "input ended while reading a u64");
  }
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(index)]) << (8 * index);
  }
  offset_ += 8;
  return Result<std::uint64_t>::success(value);
}

Result<std::int64_t> ByteReader::take_i64() {
  const auto raw = take_u64();
  if (!raw.ok()) {
    return Result<std::int64_t>::failure(raw.status());
  }
  return Result<std::int64_t>::success(static_cast<std::int64_t>(raw.value()));
}

Result<std::vector<std::uint8_t>> ByteReader::take_bytes(std::size_t length) {
  if (remaining() < length) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::TruncatedInput,
                                                      "input ended while reading a byte range");
  }
  std::vector<std::uint8_t> bytes(data_ + offset_, data_ + offset_ + length);
  offset_ += length;
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

Result<std::string> ByteReader::take_string(std::size_t max_bytes) {
  const auto length = take_u32();
  if (!length.ok()) {
    return Result<std::string>::failure(length.status());
  }
  if (length.value() > max_bytes) {
    return Result<std::string>::failure(
        ErrorCode::BoundExceeded,
        "declared string length " + std::to_string(length.value()) + " exceeds the limit of " +
            std::to_string(max_bytes));
  }
  if (remaining() < length.value()) {
    return Result<std::string>::failure(ErrorCode::TruncatedInput, "input ended inside a length-prefixed string");
  }
  std::string text(reinterpret_cast<const char*>(data_ + offset_), length.value());
  offset_ += length.value();
  return Result<std::string>::success(std::move(text));
}

Result<std::string> ByteReader::take_text(std::size_t max_bytes) {
  auto text = take_string(max_bytes);
  if (!text.ok()) {
    return text;
  }
  if (!is_valid_utf8(text.value())) {
    return Result<std::string>::failure(ErrorCode::InvalidTextEncoding, "text field is not well-formed UTF-8");
  }
  if (has_control_characters(text.value())) {
    return Result<std::string>::failure(ErrorCode::InvalidTextEncoding, "text field contains a control character");
  }
  return text;
}

}  // namespace scr
