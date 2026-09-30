#include "scr/text.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

#include "scr/canonical.hpp"
#include "scr/types.hpp"
#include "scr/version.hpp"

namespace scr {
namespace {

constexpr std::size_t kMaxDirectiveTokens = 16;

struct Line {
  std::size_t number = 0;
  std::string text;
};

Status parse_error(std::size_t line, std::string detail) {
  return Status::failure(ErrorCode::ParseError, "line " + std::to_string(line) + ": " + std::move(detail), "scx");
}

// Splits a directive into tokens. A double quoted region is never split, so a
// quoted text value or a note="..." option stays a single token even when it
// contains spaces; backslash escapes inside the quoted region are preserved for
// the quoted value parser.
std::vector<std::string> split_whitespace(const std::string& line) {
  std::vector<std::string> tokens;
  std::size_t index = 0;
  while (index < line.size()) {
    while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
      ++index;
    }
    if (index >= line.size()) {
      break;
    }
    const std::size_t start = index;
    bool quoted = false;
    while (index < line.size()) {
      const char character = line[index];
      if (quoted) {
        if (character == '\\' && index + 1 < line.size()) {
          index += 2;
          continue;
        }
        if (character == '"') {
          quoted = false;
        }
        ++index;
        continue;
      }
      if (character == '"') {
        quoted = true;
        ++index;
        continue;
      }
      if (character == ' ' || character == '\t') {
        break;
      }
      ++index;
    }
    tokens.push_back(line.substr(start, index - start));
  }
  return tokens;
}

bool is_all_digits(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  return true;
}

Result<std::uint64_t> parse_unsigned(const std::string& text) {
  if (!is_all_digits(text)) {
    return Result<std::uint64_t>::failure(
        Status::failure(ErrorCode::OutOfRange, "'" + text + "' is not an unsigned decimal integer", "scx"));
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    const auto digit = static_cast<std::uint64_t>(character - '0');
    const auto scaled = checked_mul(value, 10);
    if (!scaled.ok()) {
      return Result<std::uint64_t>::failure(scaled.status());
    }
    const auto added = checked_add(scaled.value(), digit);
    if (!added.ok()) {
      return Result<std::uint64_t>::failure(added.status());
    }
    value = added.value();
  }
  return Result<std::uint64_t>::success(value);
}

Result<RevisionRef> parse_revision_ref(const std::string& text, std::size_t line) {
  const std::size_t at = text.find('@');
  if (at == std::string::npos) {
    return Result<RevisionRef>::failure(parse_error(line, "revision reference must be generation.revision@digest"));
  }
  const std::string head = text.substr(0, at);
  const std::string digest_text = text.substr(at + 1);
  const std::size_t dot = head.find('.');
  if (dot == std::string::npos) {
    return Result<RevisionRef>::failure(parse_error(line, "revision reference must be generation.revision@digest"));
  }
  const auto generation = parse_unsigned(head.substr(0, dot));
  if (!generation.ok()) {
    return Result<RevisionRef>::failure(parse_error(line, "revision generation is not an unsigned integer"));
  }
  const auto revision = parse_unsigned(head.substr(dot + 1));
  if (!revision.ok()) {
    return Result<RevisionRef>::failure(parse_error(line, "revision number is not an unsigned integer"));
  }
  const auto digest = Digest::parse(digest_text);
  if (!digest.ok()) {
    return Result<RevisionRef>::failure(parse_error(line, "revision digest must be 64 hexadecimal characters"));
  }
  if (generation.value() == 0 || revision.value() > 0xFFFFFFFFull || revision.value() == 0) {
    return Result<RevisionRef>::failure(parse_error(line, "revision identity must be non-zero and within range"));
  }
  RevisionRef reference;
  reference.generation = Generation::from(generation.value());
  reference.revision = Revision::from(static_cast<std::uint32_t>(revision.value()));
  reference.digest = digest.value();
  return Result<RevisionRef>::success(reference);
}

Result<std::string> parse_quoted(const std::string& text, std::size_t line) {
  if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
    return Result<std::string>::failure(parse_error(line, "quoted text must start and end with a double quote"));
  }
  std::string value;
  value.reserve(text.size() - 2);
  for (std::size_t index = 1; index + 1 < text.size(); ++index) {
    const char character = text[index];
    if (character != '\\') {
      value.push_back(character);
      continue;
    }
    if (index + 2 > text.size() - 1) {
      return Result<std::string>::failure(parse_error(line, "escape sequence is truncated"));
    }
    const char escaped = text[++index];
    switch (escaped) {
      case '\\':
        value.push_back('\\');
        break;
      case '"':
        value.push_back('"');
        break;
      case 'n':
      case 'r':
      case 't':
        // Metadata values never carry control characters, so an escape that
        // would introduce one is refused rather than accepted and then
        // rejected by the value model.
        return Result<std::string>::failure(parse_error(
            line, "metadata text must not contain control characters; the escape sequence is not accepted"));
      default:
        return Result<std::string>::failure(
            parse_error(line, std::string("unsupported escape sequence '\\") + escaped + "'"));
    }
  }
  return Result<std::string>::success(std::move(value));
}

std::string escape_text(const std::string& text) {
  std::string escaped;
  escaped.reserve(text.size() + 2);
  escaped.push_back('"');
  for (const char character : text) {
    switch (character) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped.push_back(character);
        break;
    }
  }
  escaped.push_back('"');
  return escaped;
}

Result<Obligation> parse_obligation(const std::vector<std::string>& tokens, std::size_t line) {
  if (tokens.size() != 4) {
    return Result<Obligation>::failure(parse_error(line, "obligation takes exactly key, modality, and value"));
  }
  const KeyDescriptor* descriptor = find_key(tokens[1]);
  if (descriptor == nullptr) {
    return Result<Obligation>::failure(
        parse_error(line, "obligation key '" + tokens[1] + "' is not a defined service class obligation"));
  }
  const auto modality = parse_modality(tokens[2]);
  if (!modality.ok()) {
    return Result<Obligation>::failure(
        parse_error(line, "modality must be required, preferred, permitted-degraded, or unspecified"));
  }
  std::uint64_t value = 0;
  if (descriptor->kind == ValueKind::Enumerated) {
    bool found = false;
    for (std::size_t index = 0; index < descriptor->enum_count; ++index) {
      if (tokens[3] == descriptor->enum_names[index].name) {
        value = descriptor->enum_names[index].value;
        found = true;
        break;
      }
    }
    if (!found) {
      std::string allowed;
      for (std::size_t index = 0; index < descriptor->enum_count; ++index) {
        if (index != 0) {
          allowed += ", ";
        }
        allowed += descriptor->enum_names[index].name;
      }
      return Result<Obligation>::failure(parse_error(
          line, "value '" + tokens[3] + "' is not a declared enumerator of " + tokens[1] + " (" + allowed + ")"));
    }
  } else {
    const auto parsed = parse_unsigned(tokens[3]);
    if (!parsed.ok()) {
      return Result<Obligation>::failure(parse_error(line, "value for " + tokens[1] + " must be an unsigned integer"));
    }
    value = parsed.value();
  }
  return Obligation::create(descriptor->key, modality.value(), value);
}

}  // namespace

Result<ClassRevision> parse_scx(std::string_view text) {
  if (text.size() > kMaxScxBytes) {
    return Result<ClassRevision>::failure(Status::failure(
        ErrorCode::BoundExceeded,
        "the canonical text input is " + std::to_string(text.size()) + " bytes, limit is " +
            std::to_string(kMaxScxBytes),
        "scx"));
  }
  if (!is_valid_utf8(text)) {
    return Result<ClassRevision>::failure(
        Status::failure(ErrorCode::InvalidTextEncoding, "the canonical text input is not well-formed UTF-8", "scx"));
  }

  std::vector<Line> lines;
  {
    std::size_t start = 0;
    std::size_t number = 1;
    while (start <= text.size()) {
      const std::size_t end = text.find('\n', start);
      const std::size_t length = (end == std::string_view::npos) ? (text.size() - start) : (end - start);
      std::string line(text.substr(start, length));
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.size() > kMaxScxLineBytes) {
        return Result<ClassRevision>::failure(
            parse_error(number, "line exceeds the maximum canonical text line length"));
      }
      lines.push_back(Line{number, std::move(line)});
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
      ++number;
    }
  }

  ClassRevision revision;
  bool header_seen = false;
  bool class_seen = false;
  bool generation_seen = false;
  bool revision_seen = false;
  bool lineage_seen = false;
  ObligationSet obligations;
  std::vector<ExternalReference> references;
  std::vector<CompositionRef> composes;
  Metadata metadata;

  for (const Line& line : lines) {
    if (line.text.empty() || line.text[0] == '#') {
      continue;
    }
    if (!header_seen) {
      const auto tokens = split_whitespace(line.text);
      if (tokens.size() != 2 || tokens[0] != "scr-class") {
        return Result<ClassRevision>::failure(
            parse_error(line.number, "the first directive must be 'scr-class <version>'"));
      }
      const auto version = parse_unsigned(tokens[1]);
      if (!version.ok() || version.value() != kCanonicalTextVersion) {
        return Result<ClassRevision>::failure(
            parse_error(line.number, "unsupported canonical text version (this build speaks version " +
                                         std::to_string(kCanonicalTextVersion) + ")"));
      }
      header_seen = true;
      continue;
    }

    const auto tokens = split_whitespace(line.text);
    const std::string& directive = tokens[0];

    if (directive == "class") {
      if (class_seen) {
        return Result<ClassRevision>::failure(parse_error(line.number, "duplicate 'class' directive"));
      }
      if (tokens.size() != 2) {
        return Result<ClassRevision>::failure(parse_error(line.number, "class takes exactly one identity"));
      }
      const auto parsed = ClassId::parse(tokens[1]);
      if (!parsed.ok()) {
        return Result<ClassRevision>::failure(parse_error(line.number, parsed.status().detail()));
      }
      revision.class_id = parsed.value();
      class_seen = true;
      continue;
    }

    if (directive == "generation") {
      if (generation_seen) {
        return Result<ClassRevision>::failure(parse_error(line.number, "duplicate 'generation' directive"));
      }
      if (tokens.size() != 2) {
        return Result<ClassRevision>::failure(parse_error(line.number, "generation takes exactly one value"));
      }
      const auto parsed = parse_unsigned(tokens[1]);
      if (!parsed.ok() || parsed.value() == 0) {
        return Result<ClassRevision>::failure(parse_error(line.number, "generation must be a non-zero integer"));
      }
      revision.generation = Generation::from(parsed.value());
      generation_seen = true;
      continue;
    }

    if (directive == "revision") {
      if (revision_seen) {
        return Result<ClassRevision>::failure(parse_error(line.number, "duplicate 'revision' directive"));
      }
      if (tokens.size() != 2) {
        return Result<ClassRevision>::failure(parse_error(line.number, "revision takes exactly one value"));
      }
      const auto parsed = parse_unsigned(tokens[1]);
      if (!parsed.ok() || parsed.value() == 0 || parsed.value() > 0xFFFFFFFFull) {
        return Result<ClassRevision>::failure(parse_error(line.number, "revision must be a non-zero 32-bit integer"));
      }
      revision.revision = Revision::from(static_cast<std::uint32_t>(parsed.value()));
      revision_seen = true;
      continue;
    }

    if (directive == "lineage") {
      if (lineage_seen) {
        return Result<ClassRevision>::failure(parse_error(line.number, "duplicate 'lineage' directive"));
      }
      if (tokens.size() == 2 && tokens[1] == "genesis") {
        revision.lineage = Lineage::genesis();
      } else if (tokens.size() == 2) {
        const auto parsed = parse_revision_ref(tokens[1], line.number);
        if (!parsed.ok()) {
          return Result<ClassRevision>::failure(parsed.status());
        }
        const auto lineage = Lineage::from_predecessor(parsed.value());
        if (!lineage.ok()) {
          return Result<ClassRevision>::failure(parse_error(line.number, lineage.status().detail()));
        }
        revision.lineage = lineage.value();
      } else {
        return Result<ClassRevision>::failure(
            parse_error(line.number, "lineage takes 'genesis' or 'generation.revision@digest'"));
      }
      lineage_seen = true;
      continue;
    }

    if (directive == "parent") {
      if (tokens.size() != 4 || tokens[2] != "@") {
        return Result<ClassRevision>::failure(
            parse_error(line.number, "parent takes '<class-id> @ <generation.revision@digest>'"));
      }
      const auto parent = ClassId::parse(tokens[1]);
      if (!parent.ok()) {
        return Result<ClassRevision>::failure(parse_error(line.number, parent.status().detail()));
      }
      const auto reference = parse_revision_ref(tokens[3], line.number);
      if (!reference.ok()) {
        return Result<ClassRevision>::failure(reference.status());
      }
      const auto composition = CompositionRef::create(parent.value(), reference.value());
      if (!composition.ok()) {
        return Result<ClassRevision>::failure(parse_error(line.number, composition.status().detail()));
      }
      composes.push_back(composition.value());
      continue;
    }

    if (directive == "obligation") {
      const auto obligation = parse_obligation(tokens, line.number);
      if (!obligation.ok()) {
        return Result<ClassRevision>::failure(parse_error(line.number, obligation.status().detail()));
      }
      const VoidResult added = obligations.add(obligation.value());
      if (!added.ok()) {
        return Result<ClassRevision>::failure(parse_error(line.number, added.status().detail()));
      }
      continue;
    }

    if (directive == "reference") {
      if (tokens.size() < 4) {
        return Result<ClassRevision>::failure(parse_error(
            line.number, "reference takes '<kind> <target> @<digest|unbound> [authority=<id>] [note=\"...\"]'"));
      }
      const auto kind = parse_reference_kind(tokens[1]);
      if (!kind.ok()) {
        return Result<ClassRevision>::failure(parse_error(
            line.number, "reference kind must be requirement-set, policy-predicate, entitlement-profile, or "
                         "evidence-source"));
      }
      Digest digest;
      const std::string& binding = tokens[3];
      if (binding == "@unbound") {
        digest = Digest::null();
      } else if (!binding.empty() && binding[0] == '@') {
        const auto parsed = Digest::parse(binding.substr(1));
        if (!parsed.ok()) {
          return Result<ClassRevision>::failure(
              parse_error(line.number, "reference binding must be @unbound or @<64 hex characters>"));
        }
        digest = parsed.value();
      } else {
        return Result<ClassRevision>::failure(
            parse_error(line.number, "reference binding must be @unbound or @<64 hex characters>"));
      }
      AuthorityId authority;
      BoundedText note;
      for (std::size_t index = 4; index < tokens.size(); ++index) {
        const std::string& option = tokens[index];
        if (option.rfind("authority=", 0) == 0) {
          const auto parsed = AuthorityId::parse(std::string_view(option).substr(10));
          if (!parsed.ok()) {
            return Result<ClassRevision>::failure(parse_error(line.number, parsed.status().detail()));
          }
          authority = parsed.value();
          continue;
        }
        if (option.rfind("note=", 0) == 0) {
          const auto parsed = parse_quoted(option.substr(5), line.number);
          if (!parsed.ok()) {
            return Result<ClassRevision>::failure(parsed.status());
          }
          const auto created = BoundedText::create(parsed.value(), kReferenceNoteMaxBytes);
          if (!created.ok()) {
            return Result<ClassRevision>::failure(parse_error(line.number, created.status().detail()));
          }
          note = created.value();
          continue;
        }
        return Result<ClassRevision>::failure(
            parse_error(line.number, "unsupported reference option '" + option + "'"));
      }
      const auto reference = ExternalReference::create(kind.value(), tokens[2], digest, authority, note);
      if (!reference.ok()) {
        return Result<ClassRevision>::failure(parse_error(line.number, reference.status().detail()));
      }
      references.push_back(reference.value());
      continue;
    }

    if (directive == "text") {
      if (tokens.size() != 3) {
        return Result<ClassRevision>::failure(
            parse_error(line.number, "text takes a field name and a quoted value"));
      }
      const auto parsed = parse_quoted(tokens[2], line.number);
      if (!parsed.ok()) {
        return Result<ClassRevision>::failure(parsed.status());
      }
      const std::string& field = tokens[1];
      std::size_t bound = 0;
      BoundedText* target = nullptr;
      if (field == "title") {
        bound = kTitleMaxBytes;
        target = &metadata.title;
      } else if (field == "summary") {
        bound = kSummaryMaxBytes;
        target = &metadata.summary;
      } else if (field == "owner") {
        bound = kOwnerMaxBytes;
        target = &metadata.owner;
      } else if (field == "documentation") {
        bound = kDocumentationMaxBytes;
        target = &metadata.documentation;
      } else {
        return Result<ClassRevision>::failure(parse_error(line.number, "unknown text field '" + field + "'"));
      }
      if (!target->empty()) {
        return Result<ClassRevision>::failure(parse_error(line.number, "duplicate text field '" + field + "'"));
      }
      const auto created = BoundedText::create(parsed.value(), bound);
      if (!created.ok()) {
        return Result<ClassRevision>::failure(parse_error(line.number, created.status().detail()));
      }
      *target = created.value();
      continue;
    }

    return Result<ClassRevision>::failure(parse_error(line.number, "unknown directive '" + directive + "'"));
  }

  if (!header_seen) {
    return Result<ClassRevision>::failure(
        Status::failure(ErrorCode::ParseError, "the canonical text input is empty", "scx"));
  }
  if (!class_seen) {
    return Result<ClassRevision>::failure(
        Status::failure(ErrorCode::ParseError, "the 'class' directive is required", "scx"));
  }
  if (!generation_seen) {
    return Result<ClassRevision>::failure(
        Status::failure(ErrorCode::ParseError, "the 'generation' directive is required", "scx"));
  }
  if (!revision_seen) {
    return Result<ClassRevision>::failure(
        Status::failure(ErrorCode::ParseError, "the 'revision' directive is required", "scx"));
  }

  return make_revision(revision.class_id, revision.generation, revision.revision, revision.lineage,
                       std::move(obligations), std::move(references), std::move(composes), std::move(metadata));
}

std::string emit_scx(const ClassRevision& revision) {
  std::string out;
  out.reserve(512);
  out += "scr-class ";
  out += std::to_string(kCanonicalTextVersion);
  out += "\nclass ";
  out += revision.class_id.str();
  out += "\ngeneration ";
  out += std::to_string(revision.generation.value());
  out += "\nrevision ";
  out += std::to_string(revision.revision.value());
  out += "\nlineage ";
  if (revision.lineage.is_genesis()) {
    out += "genesis";
  } else {
    out += revision.lineage.predecessor().to_string();
  }
  out += "\n";

  for (const CompositionRef& composition : revision.composes) {
    out += "parent ";
    out += composition.parent.str();
    out += " @ ";
    out += composition.revision.to_string();
    out += "\n";
  }

  if (!revision.metadata.title.empty()) {
    out += "text title ";
    out += escape_text(revision.metadata.title.value());
    out += "\n";
  }
  if (!revision.metadata.summary.empty()) {
    out += "text summary ";
    out += escape_text(revision.metadata.summary.value());
    out += "\n";
  }
  if (!revision.metadata.owner.empty()) {
    out += "text owner ";
    out += escape_text(revision.metadata.owner.value());
    out += "\n";
  }
  if (!revision.metadata.documentation.empty()) {
    out += "text documentation ";
    out += escape_text(revision.metadata.documentation.value());
    out += "\n";
  }

  for (const ExternalReference& reference : revision.references) {
    out += "reference ";
    out += to_string(reference.kind);
    out += " ";
    out += reference.target;
    out += " @";
    out += reference.is_bound() ? reference.digest.hex() : std::string("unbound");
    if (!reference.authority.empty()) {
      out += " authority=";
      out += reference.authority.str();
    }
    if (!reference.note.empty()) {
      out += " note=";
      out += escape_text(reference.note.value());
    }
    out += "\n";
  }

  for (const auto& entry : revision.obligations.items()) {
    const Obligation& obligation = entry.second;
    out += "obligation ";
    out += describe(obligation.key()).name;
    out += " ";
    out += to_string(obligation.modality());
    out += " ";
    out += obligation.value_text();
    out += "\n";
  }
  return out;
}

std::string emit_frame_hex(const ClassRevision& revision) {
  const auto frame = encode_revision_frame(revision);
  if (!frame.ok()) {
    return std::string();
  }
  static const char* kDigits = "0123456789abcdef";
  std::string hex;
  hex.reserve(frame.value().size() * 2);
  for (const std::uint8_t byte : frame.value()) {
    hex.push_back(kDigits[(byte >> 4) & 0x0F]);
    hex.push_back(kDigits[byte & 0x0F]);
  }
  return hex;
}

Result<ClassRevision> parse_frame_hex(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    return Result<ClassRevision>::failure(Status::failure(
        ErrorCode::InvalidArgument, "the canonical frame hex form must have an even number of characters", "frame"));
  }
  if (hex.size() > (kFrameOverheadBytes + kMaxRevisionPayloadBytes) * 2) {
    return Result<ClassRevision>::failure(Status::failure(
        ErrorCode::BoundExceeded, "the canonical frame hex form exceeds the maximum frame size", "frame"));
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    const auto nibble = [](char character) -> int {
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
    };
    const int high = nibble(hex[index]);
    const int low = nibble(hex[index + 1]);
    if (high < 0 || low < 0) {
      return Result<ClassRevision>::failure(Status::failure(
          ErrorCode::InvalidArgument, "the canonical frame hex form contains a non-hexadecimal character", "frame"));
    }
    bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return decode_revision_frame(bytes.data(), bytes.size());
}

}  // namespace scr
