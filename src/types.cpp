#include "scr/types.hpp"

#include <algorithm>
#include <limits>

namespace scr {
namespace {

inline bool is_lower_alnum(char character) noexcept {
  return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
}

inline bool is_token_tail(char character) noexcept {
  return is_lower_alnum(character) || character == '.' || character == '_' || character == '-';
}

struct CivilDate {
  std::int64_t year;
  unsigned month;
  unsigned day;
};

// Howard Hinnant's civil_from_days, valid for the full range of days used here.
CivilDate civil_from_days(std::int64_t days) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  std::int64_t year = static_cast<std::int64_t>(year_of_era) + era * 400;
  const unsigned day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const unsigned month_prime = (5 * day_of_year + 2) / 153;
  const unsigned day = day_of_year - (153 * month_prime + 2) / 5 + 1;
  const unsigned month = month_prime < 10 ? month_prime + 3 : month_prime - 9;
  year += (month <= 2) ? 1 : 0;
  return CivilDate{year, month, day};
}

void append_two_digits(std::string& text, unsigned value) {
  text.push_back(static_cast<char>('0' + (value / 10) % 10));
  text.push_back(static_cast<char>('0' + value % 10));
}

}  // namespace

Result<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return Result<std::uint64_t>::failure(ErrorCode::Overflow, "unsigned 64-bit addition would overflow");
  }
  return Result<std::uint64_t>::success(a + b);
}

Result<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b) {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return Result<std::uint64_t>::failure(ErrorCode::Overflow, "unsigned 64-bit multiplication would overflow");
  }
  return Result<std::uint64_t>::success(a * b);
}

bool is_valid_utf8(std::string_view text) noexcept {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  const std::size_t size = text.size();
  std::size_t index = 0;
  while (index < size) {
    const unsigned char lead = bytes[index];
    if (lead == 0x00u) {
      return false;
    }
    if (lead < 0x80u) {
      ++index;
      continue;
    }
    std::size_t continuation = 0;
    std::uint32_t code_point = 0;
    if ((lead & 0xE0u) == 0xC0u) {
      continuation = 1;
      code_point = static_cast<std::uint32_t>(lead & 0x1Fu);
    } else if ((lead & 0xF0u) == 0xE0u) {
      continuation = 2;
      code_point = static_cast<std::uint32_t>(lead & 0x0Fu);
    } else if ((lead & 0xF8u) == 0xF0u) {
      continuation = 3;
      code_point = static_cast<std::uint32_t>(lead & 0x07u);
    } else {
      return false;
    }
    if (size - index - 1 < continuation) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation; ++offset) {
      const unsigned char next = bytes[index + offset];
      if ((next & 0xC0u) != 0x80u) {
        return false;
      }
      code_point = (code_point << 6) | static_cast<std::uint32_t>(next & 0x3Fu);
    }
    if (continuation == 1 && code_point < 0x80u) {
      return false;
    }
    if (continuation == 2 && code_point < 0x800u) {
      return false;
    }
    if (continuation == 3 && code_point < 0x10000u) {
      return false;
    }
    if (code_point > 0x10FFFFu) {
      return false;
    }
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
      return false;
    }
    index += continuation + 1;
  }
  return true;
}

bool has_control_characters(std::string_view text) noexcept {
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20u || byte == 0x7Fu) {
      return true;
    }
  }
  return false;
}

Result<BoundedText> BoundedText::create(std::string value, std::size_t max_bytes) {
  if (value.empty()) {
    return Result<BoundedText>::failure(ErrorCode::InvalidArgument,
                                        "empty text is not a value; omit the field to leave it unset", "text");
  }
  if (value.size() > max_bytes) {
    return Result<BoundedText>::failure(
        ErrorCode::BoundExceeded,
        "text is " + std::to_string(value.size()) + " bytes, limit is " + std::to_string(max_bytes), "text");
  }
  if (!is_valid_utf8(value)) {
    return Result<BoundedText>::failure(ErrorCode::InvalidTextEncoding, "text is not well-formed UTF-8", "text");
  }
  if (has_control_characters(value)) {
    return Result<BoundedText>::failure(ErrorCode::InvalidTextEncoding,
                                        "text contains a control character", "text");
  }
  BoundedText text;
  text.value_ = std::move(value);
  return Result<BoundedText>::success(std::move(text));
}

bool is_valid_identifier_token(std::string_view token) noexcept {
  if (token.empty() || token.size() > kClassIdTokenMaxBytes) {
    return false;
  }
  if (!is_lower_alnum(token.front()) || !is_lower_alnum(token.back())) {
    return false;
  }
  for (std::size_t index = 0; index < token.size(); ++index) {
    if (!is_token_tail(token[index])) {
      return false;
    }
  }
  return token.find("..") == std::string_view::npos;
}

bool is_valid_identifier_path(std::string_view text, std::size_t max_segments) noexcept {
  if (text.empty() || text.size() > kReferenceTargetMaxBytes) {
    return false;
  }
  std::size_t segments = 0;
  std::size_t start = 0;
  while (true) {
    const std::size_t separator = text.find('/', start);
    const std::string_view segment =
        (separator == std::string_view::npos) ? text.substr(start) : text.substr(start, separator - start);
    if (!is_valid_identifier_token(segment)) {
      return false;
    }
    ++segments;
    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1;
  }
  return segments <= max_segments;
}

Result<ClassId> ClassId::parse(std::string_view text) {
  if (text.empty()) {
    return Result<ClassId>::failure(ErrorCode::InvalidIdentifier, "class identity is empty", "class");
  }
  if (text.size() > kClassIdMaxBytes) {
    return Result<ClassId>::failure(
        ErrorCode::InvalidIdentifier,
        "class identity is " + std::to_string(text.size()) + " bytes, limit is " + std::to_string(kClassIdMaxBytes),
        "class");
  }
  const std::size_t separator = text.find('/');
  if (separator == std::string_view::npos) {
    return Result<ClassId>::failure(ErrorCode::InvalidIdentifier,
                                    "class identity must be namespace/name", "class");
  }
  if (text.find('/', separator + 1) != std::string_view::npos) {
    return Result<ClassId>::failure(ErrorCode::InvalidIdentifier,
                                    "class identity must contain exactly one separator", "class");
  }
  const std::string_view name_space = text.substr(0, separator);
  const std::string_view name = text.substr(separator + 1);
  if (!is_valid_identifier_token(name_space) || !is_valid_identifier_token(name)) {
    return Result<ClassId>::failure(
        ErrorCode::InvalidIdentifier,
        "class identity tokens must be lower-case ASCII tokens of at most 63 characters, starting and ending with a "
        "letter or digit",
        "class");
  }
  ClassId id;
  id.text_.assign(text);
  return Result<ClassId>::success(std::move(id));
}

Result<ClassId> ClassId::compose(std::string_view name_space, std::string_view name) {
  std::string text;
  text.reserve(name_space.size() + 1 + name.size());
  text.append(name_space);
  text.push_back('/');
  text.append(name);
  return parse(text);
}

std::string_view ClassId::name_space() const noexcept {
  const std::size_t separator = text_.find('/');
  if (separator == std::string::npos) {
    return std::string_view();
  }
  return std::string_view(text_).substr(0, separator);
}

std::string_view ClassId::name() const noexcept {
  const std::size_t separator = text_.find('/');
  if (separator == std::string::npos) {
    return std::string_view();
  }
  return std::string_view(text_).substr(separator + 1);
}

Result<AuthorityId> AuthorityId::parse(std::string_view text) {
  if (text.empty()) {
    return Result<AuthorityId>::failure(ErrorCode::InvalidIdentifier, "authority identity is empty", "authority");
  }
  if (text.size() > kAuthorityIdMaxBytes) {
    return Result<AuthorityId>::failure(ErrorCode::InvalidIdentifier,
                                        "authority identity exceeds the 64 byte limit", "authority");
  }
  const auto is_head = [](char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9');
  };
  const auto is_tail = [&is_head](char character) noexcept {
    return is_head(character) || character == '.' || character == '_' || character == '-';
  };
  if (!is_head(text.front()) || !is_head(text.back())) {
    return Result<AuthorityId>::failure(ErrorCode::InvalidIdentifier,
                                        "authority identity must start and end with a letter or digit",
                                        "authority");
  }
  for (const char character : text) {
    if (!is_tail(character)) {
      return Result<AuthorityId>::failure(ErrorCode::InvalidIdentifier,
                                          "authority identity contains an unsupported character", "authority");
    }
  }
  AuthorityId authority;
  authority.text_.assign(text);
  return Result<AuthorityId>::success(std::move(authority));
}

Result<Generation> Generation::next() const {
  if (value_ == std::numeric_limits<std::uint64_t>::max()) {
    return Result<Generation>::failure(ErrorCode::Overflow, "generation counter is exhausted");
  }
  return Result<Generation>::success(Generation(value_ + 1));
}

Result<Revision> Revision::next() const {
  if (value_ == std::numeric_limits<std::uint32_t>::max()) {
    return Result<Revision>::failure(ErrorCode::Overflow, "revision counter is exhausted");
  }
  return Result<Revision>::success(Revision(value_ + 1));
}

Result<Sequence> Sequence::next() const {
  if (value_ == std::numeric_limits<std::uint64_t>::max()) {
    return Result<Sequence>::failure(ErrorCode::Overflow, "manifest sequence counter is exhausted");
  }
  return Result<Sequence>::success(Sequence(value_ + 1));
}

Result<Timestamp> Timestamp::from_unix_seconds(std::int64_t seconds) {
  if (seconds < 0 || seconds > kMaxUnixSeconds) {
    return Result<Timestamp>::failure(ErrorCode::OutOfRange,
                                      "timestamp must be within [0, 253402300799] (1970-01-01 to 9999-12-31)",
                                      "timestamp");
  }
  Timestamp timestamp;
  timestamp.seconds_ = seconds;
  return Result<Timestamp>::success(timestamp);
}

std::string Timestamp::to_iso8601() const {
  if (!seconds_.has_value()) {
    return "unset";
  }
  const std::int64_t seconds = *seconds_;
  const std::int64_t days = seconds / 86400;
  const std::int64_t remainder = seconds % 86400;
  const CivilDate date = civil_from_days(days);

  std::string text;
  text.reserve(20);
  const std::int64_t year = date.year;
  text.push_back(static_cast<char>('0' + (year / 1000) % 10));
  text.push_back(static_cast<char>('0' + (year / 100) % 10));
  text.push_back(static_cast<char>('0' + (year / 10) % 10));
  text.push_back(static_cast<char>('0' + year % 10));
  text.push_back('-');
  append_two_digits(text, date.month);
  text.push_back('-');
  append_two_digits(text, date.day);
  text.push_back('T');
  append_two_digits(text, static_cast<unsigned>(remainder / 3600));
  text.push_back(':');
  append_two_digits(text, static_cast<unsigned>((remainder / 60) % 60));
  text.push_back(':');
  append_two_digits(text, static_cast<unsigned>(remainder % 60));
  text.push_back('Z');
  return text;
}

std::string RevisionRef::to_string() const {
  return std::to_string(generation.value()) + "." + std::to_string(revision.value()) + "@" + digest.hex();
}

std::string RevisionRef::to_display_string() const {
  return std::to_string(generation.value()) + "." + std::to_string(revision.value()) + "@" + digest.short_hex();
}

std::string ClassBinding::to_string() const {
  return class_id.str() + "#" + revision.to_string();
}

}  // namespace scr
