#include "scr/digest.hpp"

#include <cstring>

namespace scr {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr std::array<std::uint32_t, 8> kInitialState = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

constexpr char kHexDigits[] = "0123456789abcdef";

inline std::uint32_t rotr(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32u - count));
}

inline std::uint32_t load_be32(const std::uint8_t* data) noexcept {
  return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

inline void store_be32(std::uint8_t* out, std::uint32_t value) noexcept {
  out[0] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
  out[1] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  out[2] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  out[3] = static_cast<std::uint8_t>(value & 0xFFu);
}

inline void store_be64(std::uint8_t* out, std::uint64_t value) noexcept {
  for (int index = 0; index < 8; ++index) {
    out[index] = static_cast<std::uint8_t>((value >> (56 - 8 * index)) & 0xFFu);
  }
}

int hex_value(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

}  // namespace

Sha256::Sha256() noexcept {
  state_ = kInitialState;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::uint32_t schedule[64];
  for (int index = 0; index < 16; ++index) {
    schedule[index] = load_be32(block + 4 * index);
  }
  for (int index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15], 7) ^ rotr(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3);
    const std::uint32_t s1 = rotr(schedule[index - 2], 17) ^ rotr(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (int index = 0; index < 64; ++index) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choice = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + choice + kRoundConstants[static_cast<std::size_t>(index)] + schedule[index];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t length) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += static_cast<std::uint64_t>(length);
  while (length > 0) {
    const std::size_t room = kBlockBytes - buffered_;
    const std::size_t take = (length < room) ? length : room;
    std::memcpy(buffer_.data() + buffered_, bytes, take);
    buffered_ += take;
    bytes += take;
    length -= take;
    if (buffered_ == kBlockBytes) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
}

void Sha256::update(std::string_view text) noexcept {
  update(text.data(), text.size());
}

std::array<std::uint8_t, Sha256::kDigestBytes> Sha256::finish() noexcept {
  const std::uint64_t total_bits = total_bytes_ * 8u;
  const std::uint8_t padding = 0x80u;
  update(&padding, 1);

  const std::uint8_t zero = 0x00u;
  while (buffered_ != 56) {
    update(&zero, 1);
  }

  std::uint8_t length_bytes[8];
  store_be64(length_bytes, total_bits);
  update(length_bytes, 8);

  std::array<std::uint8_t, kDigestBytes> digest{};
  for (std::size_t index = 0; index < 8; ++index) {
    store_be32(digest.data() + 4 * index, state_[index]);
  }

  state_ = kInitialState;
  buffered_ = 0;
  total_bytes_ = 0;
  return digest;
}

std::array<std::uint8_t, Sha256::kDigestBytes> sha256(const void* data, std::size_t length) noexcept {
  Sha256 hasher;
  hasher.update(data, length);
  return hasher.finish();
}

std::array<std::uint8_t, Sha256::kDigestBytes> sha256(std::string_view text) noexcept {
  return sha256(text.data(), text.size());
}

std::uint32_t crc32(const void* data, std::size_t length) noexcept {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> values{};
    for (std::uint32_t index = 0; index < 256; ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
      }
      values[index] = value;
    }
    return values;
  }();

  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t index = 0; index < length; ++index) {
    crc = table[(crc ^ bytes[index]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint32_t crc32(std::string_view text) noexcept {
  return crc32(text.data(), text.size());
}

Digest Digest::from_bytes(const std::array<std::uint8_t, kBytes>& bytes) noexcept {
  Digest digest;
  digest.bytes_ = bytes;
  return digest;
}

Digest Digest::of(const void* data, std::size_t length) noexcept {
  return from_bytes(sha256(data, length));
}

Digest Digest::of(std::string_view text) noexcept {
  return from_bytes(sha256(text));
}

Result<Digest> Digest::parse(std::string_view hex) {
  if (hex.size() != kHexChars) {
    return Result<Digest>::failure(ErrorCode::InvalidArgument,
                                   "a digest must be exactly 64 hexadecimal characters, got " +
                                       std::to_string(hex.size()),
                                   "digest");
  }
  std::array<std::uint8_t, kBytes> bytes{};
  for (std::size_t index = 0; index < kBytes; ++index) {
    const int high = hex_value(hex[2 * index]);
    const int low = hex_value(hex[2 * index + 1]);
    if (high < 0 || low < 0) {
      return Result<Digest>::failure(ErrorCode::InvalidArgument,
                                     "a digest must contain only hexadecimal characters", "digest");
    }
    bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return Result<Digest>::success(from_bytes(bytes));
}

bool Digest::is_null() const noexcept {
  for (const auto byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

std::string Digest::hex() const {
  std::string text(kHexChars, '0');
  for (std::size_t index = 0; index < kBytes; ++index) {
    text[2 * index] = kHexDigits[(bytes_[index] >> 4) & 0x0Fu];
    text[2 * index + 1] = kHexDigits[bytes_[index] & 0x0Fu];
  }
  return text;
}

std::string Digest::short_hex() const {
  return hex().substr(0, 12);
}

}  // namespace scr
