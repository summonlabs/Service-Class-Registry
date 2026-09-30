#include "scr/canonical.hpp"

#include <array>
#include <cstring>

#include "scr/bytes.hpp"
#include "scr/version.hpp"

namespace scr {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic = {'S', 'C', 'R', 'F'};
constexpr std::uint8_t kFlagsMustBeZero = 0;
constexpr std::uint16_t kRevisionPayloadVersion = 1;
constexpr std::uint16_t kGuardPayloadVersion = 1;
constexpr std::uint8_t kLineageGenesis = 0;
constexpr std::uint8_t kLineagePredecessor = 1;

void store_u16(std::uint8_t* out, std::uint16_t value) noexcept {
  out[0] = static_cast<std::uint8_t>(value & 0xFFu);
  out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void store_u32(std::uint8_t* out, std::uint32_t value) noexcept {
  for (int index = 0; index < 4; ++index) {
    out[index] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
  }
}

std::uint16_t load_u16(const std::uint8_t* data) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8));
}

std::uint32_t load_u32(const std::uint8_t* data) noexcept {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data[index]) << (8 * index);
  }
  return value;
}

Status corrupt(std::string detail, std::string subject) {
  return Status::failure(ErrorCode::CorruptStore, std::move(detail), std::move(subject));
}

void put_digest(ByteWriter& writer, const Digest& digest) {
  writer.put_bytes(digest.bytes().data(), digest.bytes().size());
}

Result<Digest> take_digest(ByteReader& reader) {
  auto bytes = reader.take_bytes(Digest::kBytes);
  if (!bytes.ok()) {
    return Result<Digest>::failure(bytes.status());
  }
  std::array<std::uint8_t, Digest::kBytes> raw{};
  std::memcpy(raw.data(), bytes.value().data(), raw.size());
  return Result<Digest>::success(Digest::from_bytes(raw));
}

}  // namespace

const char* to_string(FrameKind kind) noexcept {
  switch (kind) {
    case FrameKind::RevisionContent:
      return "revision-content";
    case FrameKind::StoreManifest:
      return "store-manifest";
    case FrameKind::StoreGuard:
      return "store-guard";
  }
  return "unknown";
}

Result<std::vector<std::uint8_t>> encode_frame(FrameKind kind, const std::vector<std::uint8_t>& payload,
                                               std::uint32_t max_payload) {
  if (payload.size() > max_payload) {
    return Result<std::vector<std::uint8_t>>::failure(
        ErrorCode::BoundExceeded,
        "payload of " + std::to_string(payload.size()) + " bytes exceeds the limit of " +
            std::to_string(max_payload) + " bytes",
        to_string(kind));
  }
  std::vector<std::uint8_t> frame(kFrameOverheadBytes + payload.size(), 0);
  std::memcpy(frame.data(), kMagic.data(), kMagic.size());
  store_u16(frame.data() + 4, kFrameFormatVersion);
  frame[6] = static_cast<std::uint8_t>(kind);
  frame[7] = kFlagsMustBeZero;
  store_u32(frame.data() + 8, static_cast<std::uint32_t>(payload.size()));
  std::memcpy(frame.data() + kFrameHeaderBytes, payload.data(), payload.size());
  const std::uint32_t crc = crc32(frame.data(), kFrameHeaderBytes + payload.size());
  store_u32(frame.data() + kFrameHeaderBytes + payload.size(), crc);
  return Result<std::vector<std::uint8_t>>::success(std::move(frame));
}

Result<DecodedFrame> decode_frame(const std::uint8_t* data, std::size_t size, FrameKind expected_kind,
                                  std::uint32_t max_payload) {
  if (data == nullptr) {
    return Result<DecodedFrame>::failure(ErrorCode::InvalidArgument, "frame pointer is null", "frame");
  }
  if (size < kFrameOverheadBytes) {
    return Result<DecodedFrame>::failure(
        ErrorCode::TruncatedInput,
        "frame is " + std::to_string(size) + " bytes, the minimum frame is " + std::to_string(kFrameOverheadBytes),
        "frame");
  }
  if (std::memcmp(data, kMagic.data(), kMagic.size()) != 0) {
    return Result<DecodedFrame>::failure(corrupt("frame magic is not SCRF", "frame"));
  }
  const std::uint16_t version = load_u16(data + 4);
  if (version != kFrameFormatVersion) {
    return Result<DecodedFrame>::failure(
        ErrorCode::UnsupportedVersion,
        "frame format version " + std::to_string(version) + " is not supported (this build speaks version " +
            std::to_string(kFrameFormatVersion) + ")",
        "frame");
  }
  const auto kind = static_cast<FrameKind>(data[6]);
  if (kind != expected_kind) {
    return Result<DecodedFrame>::failure(ErrorCode::InvalidArgument,
                                         std::string("frame kind is '") + to_string(kind) + "', expected '" +
                                             to_string(expected_kind) + "'",
                                         "frame");
  }
  if (data[7] != kFlagsMustBeZero) {
    return Result<DecodedFrame>::failure(corrupt("reserved frame flags are not zero", "frame"));
  }
  const std::uint32_t payload_length = load_u32(data + 8);
  if (payload_length > max_payload) {
    return Result<DecodedFrame>::failure(
        ErrorCode::BoundExceeded,
        "declared payload length " + std::to_string(payload_length) + " exceeds the limit of " +
            std::to_string(max_payload),
        "frame");
  }
  const std::size_t expected_size = kFrameOverheadBytes + static_cast<std::size_t>(payload_length);
  if (size != expected_size) {
    return Result<DecodedFrame>::failure(
        ErrorCode::TruncatedInput,
        "frame declares " + std::to_string(payload_length) + " payload bytes but the input is " +
            std::to_string(size) + " bytes (expected exactly " + std::to_string(expected_size) + ")",
        "frame");
  }
  const std::uint32_t stored_crc = load_u32(data + kFrameHeaderBytes + payload_length);
  const std::uint32_t computed_crc = crc32(data, kFrameHeaderBytes + payload_length);
  if (stored_crc != computed_crc) {
    return Result<DecodedFrame>::failure(corrupt("frame CRC does not match its contents", "frame"));
  }

  DecodedFrame frame;
  frame.kind = kind;
  frame.format_version = version;
  frame.payload.assign(data + kFrameHeaderBytes, data + kFrameHeaderBytes + payload_length);
  frame.frame_digest = Digest::of(data, size);
  return Result<DecodedFrame>::success(std::move(frame));
}

Result<std::vector<std::uint8_t>> encode_revision_payload(const ClassRevision& revision) {
  const Status validation = revision.validate();
  if (!validation.ok()) {
    return Result<std::vector<std::uint8_t>>::failure(validation);
  }

  ByteWriter writer;
  writer.put_u16(kRevisionPayloadVersion);
  writer.put_u8(0);
  writer.put_string(revision.class_id.str());
  writer.put_u64(revision.generation.value());
  writer.put_u32(revision.revision.value());
  if (revision.lineage.is_genesis()) {
    writer.put_u8(kLineageGenesis);
  } else {
    writer.put_u8(kLineagePredecessor);
    writer.put_u64(revision.lineage.predecessor().generation.value());
    writer.put_u32(revision.lineage.predecessor().revision.value());
    put_digest(writer, revision.lineage.predecessor().digest);
  }

  writer.put_u16(static_cast<std::uint16_t>(revision.obligations.size()));
  for (const auto& entry : revision.obligations.items()) {
    writer.put_u16(static_cast<std::uint16_t>(entry.first));
    writer.put_u8(static_cast<std::uint8_t>(entry.second.modality()));
    writer.put_u64(entry.second.value());
  }

  writer.put_u16(static_cast<std::uint16_t>(revision.references.size()));
  for (const ExternalReference& reference : revision.references) {
    writer.put_u8(static_cast<std::uint8_t>(reference.kind));
    writer.put_string(reference.target);
    put_digest(writer, reference.digest);
    writer.put_u8(reference.authority.empty() ? 0 : 1);
    if (!reference.authority.empty()) {
      writer.put_string(reference.authority.str());
    }
    writer.put_string(reference.note.value());
  }

  writer.put_u16(static_cast<std::uint16_t>(revision.composes.size()));
  for (const CompositionRef& composition : revision.composes) {
    writer.put_string(composition.parent.str());
    writer.put_u64(composition.revision.generation.value());
    writer.put_u32(composition.revision.revision.value());
    put_digest(writer, composition.revision.digest);
  }

  writer.put_string(revision.metadata.title.value());
  writer.put_string(revision.metadata.summary.value());
  writer.put_string(revision.metadata.owner.value());
  writer.put_string(revision.metadata.documentation.value());

  return Result<std::vector<std::uint8_t>>::success(writer.take());
}

Result<ClassRevision> decode_revision_payload(const std::vector<std::uint8_t>& payload) {
  ByteReader reader(payload.data(), payload.size());

  const auto version = reader.take_u16();
  if (!version.ok()) {
    return Result<ClassRevision>::failure(version.status());
  }
  if (version.value() != kRevisionPayloadVersion) {
    return Result<ClassRevision>::failure(
        ErrorCode::UnsupportedVersion,
        "revision payload version " + std::to_string(version.value()) + " is not supported", "revision");
  }
  const auto reserved = reader.take_u8();
  if (!reserved.ok()) {
    return Result<ClassRevision>::failure(reserved.status());
  }
  if (reserved.value() != 0) {
    return Result<ClassRevision>::failure(corrupt("reserved revision payload byte is not zero", "revision"));
  }

  ClassRevision revision;
  {
    const auto text = reader.take_string(kClassIdMaxBytes);
    if (!text.ok()) {
      return Result<ClassRevision>::failure(text.status());
    }
    const auto parsed = ClassId::parse(text.value());
    if (!parsed.ok()) {
      return Result<ClassRevision>::failure(parsed.status());
    }
    revision.class_id = parsed.value();
  }
  {
    const auto generation = reader.take_u64();
    if (!generation.ok()) {
      return Result<ClassRevision>::failure(generation.status());
    }
    revision.generation = Generation::from(generation.value());
  }
  {
    const auto number = reader.take_u32();
    if (!number.ok()) {
      return Result<ClassRevision>::failure(number.status());
    }
    revision.revision = Revision::from(number.value());
  }
  {
    const auto lineage_kind = reader.take_u8();
    if (!lineage_kind.ok()) {
      return Result<ClassRevision>::failure(lineage_kind.status());
    }
    if (lineage_kind.value() == kLineageGenesis) {
      revision.lineage = Lineage::genesis();
    } else if (lineage_kind.value() == kLineagePredecessor) {
      const auto generation = reader.take_u64();
      const auto number = reader.take_u32();
      if (!generation.ok()) {
        return Result<ClassRevision>::failure(generation.status());
      }
      if (!number.ok()) {
        return Result<ClassRevision>::failure(number.status());
      }
      const auto digest = take_digest(reader);
      if (!digest.ok()) {
        return Result<ClassRevision>::failure(digest.status());
      }
      RevisionRef predecessor;
      predecessor.generation = Generation::from(generation.value());
      predecessor.revision = Revision::from(number.value());
      predecessor.digest = digest.value();
      const auto lineage = Lineage::from_predecessor(predecessor);
      if (!lineage.ok()) {
        return Result<ClassRevision>::failure(lineage.status());
      }
      revision.lineage = lineage.value();
    } else {
      return Result<ClassRevision>::failure(corrupt("lineage discriminator is not defined", "lineage"));
    }
  }

  {
    const auto count = reader.take_u16();
    if (!count.ok()) {
      return Result<ClassRevision>::failure(count.status());
    }
    if (count.value() > kMaxObligationsPerRevision) {
      return Result<ClassRevision>::failure(ErrorCode::BoundExceeded,
                                            "declared obligation count exceeds the limit", "obligations");
    }
    for (std::uint16_t index = 0; index < count.value(); ++index) {
      const auto key = reader.take_u16();
      const auto modality = reader.take_u8();
      const auto value = reader.take_u64();
      if (!key.ok()) {
        return Result<ClassRevision>::failure(key.status());
      }
      if (!modality.ok()) {
        return Result<ClassRevision>::failure(modality.status());
      }
      if (!value.ok()) {
        return Result<ClassRevision>::failure(value.status());
      }
      if (!is_valid_obligation_key(key.value())) {
        return Result<ClassRevision>::failure(ErrorCode::InvalidEnum,
                                              "obligation key " + std::to_string(key.value()) + " is not defined",
                                              "obligations");
      }
      if (modality.value() > static_cast<std::uint8_t>(Modality::Required)) {
        return Result<ClassRevision>::failure(ErrorCode::InvalidEnum, "modality value is not defined", "obligations");
      }
      const auto obligation = Obligation::create(static_cast<ObligationKey>(key.value()),
                                                 static_cast<Modality>(modality.value()), value.value());
      if (!obligation.ok()) {
        return Result<ClassRevision>::failure(obligation.status());
      }
      const VoidResult added = revision.obligations.add(obligation.value());
      if (!added.ok()) {
        return Result<ClassRevision>::failure(added.status());
      }
    }
  }

  {
    const auto count = reader.take_u16();
    if (!count.ok()) {
      return Result<ClassRevision>::failure(count.status());
    }
    if (count.value() > kMaxReferencesPerRevision) {
      return Result<ClassRevision>::failure(ErrorCode::BoundExceeded,
                                            "declared reference count exceeds the limit", "references");
    }
    for (std::uint16_t index = 0; index < count.value(); ++index) {
      const auto kind = reader.take_u8();
      if (!kind.ok()) {
        return Result<ClassRevision>::failure(kind.status());
      }
      if (kind.value() < 1 || kind.value() > static_cast<std::uint8_t>(ReferenceKind::EvidenceSource)) {
        return Result<ClassRevision>::failure(ErrorCode::InvalidEnum, "reference kind is not defined", "references");
      }
      const auto target = reader.take_string(kReferenceTargetMaxBytes);
      if (!target.ok()) {
        return Result<ClassRevision>::failure(target.status());
      }
      const auto digest = take_digest(reader);
      if (!digest.ok()) {
        return Result<ClassRevision>::failure(digest.status());
      }
      const auto has_authority = reader.take_u8();
      if (!has_authority.ok()) {
        return Result<ClassRevision>::failure(has_authority.status());
      }
      if (has_authority.value() > 1) {
        return Result<ClassRevision>::failure(corrupt("reference authority discriminator is not defined",
                                                      "references"));
      }
      AuthorityId authority;
      if (has_authority.value() == 1) {
        const auto text = reader.take_string(kAuthorityIdMaxBytes);
        if (!text.ok()) {
          return Result<ClassRevision>::failure(text.status());
        }
        const auto parsed = AuthorityId::parse(text.value());
        if (!parsed.ok()) {
          return Result<ClassRevision>::failure(parsed.status());
        }
        authority = parsed.value();
      }
      const auto note = reader.take_text(kReferenceNoteMaxBytes);
      if (!note.ok()) {
        return Result<ClassRevision>::failure(note.status());
      }
      BoundedText note_text;
      if (!note.value().empty()) {
        const auto created = BoundedText::create(note.value(), kReferenceNoteMaxBytes);
        if (!created.ok()) {
          return Result<ClassRevision>::failure(created.status());
        }
        note_text = created.value();
      }
      ExternalReference reference;
      reference.kind = static_cast<ReferenceKind>(kind.value());
      reference.target = target.value();
      reference.digest = digest.value();
      reference.authority = authority;
      reference.note = note_text;
      revision.references.push_back(std::move(reference));
    }
  }

  {
    const auto count = reader.take_u16();
    if (!count.ok()) {
      return Result<ClassRevision>::failure(count.status());
    }
    if (count.value() > kMaxCompositionsPerRevision) {
      return Result<ClassRevision>::failure(ErrorCode::BoundExceeded,
                                            "declared composition count exceeds the limit", "composes");
    }
    for (std::uint16_t index = 0; index < count.value(); ++index) {
      const auto parent_text = reader.take_string(kClassIdMaxBytes);
      if (!parent_text.ok()) {
        return Result<ClassRevision>::failure(parent_text.status());
      }
      const auto parent = ClassId::parse(parent_text.value());
      if (!parent.ok()) {
        return Result<ClassRevision>::failure(parent.status());
      }
      const auto generation = reader.take_u64();
      const auto number = reader.take_u32();
      if (!generation.ok()) {
        return Result<ClassRevision>::failure(generation.status());
      }
      if (!number.ok()) {
        return Result<ClassRevision>::failure(number.status());
      }
      const auto digest = take_digest(reader);
      if (!digest.ok()) {
        return Result<ClassRevision>::failure(digest.status());
      }
      CompositionRef composition;
      composition.parent = parent.value();
      composition.revision.generation = Generation::from(generation.value());
      composition.revision.revision = Revision::from(number.value());
      composition.revision.digest = digest.value();
      revision.composes.push_back(std::move(composition));
    }
  }

  {
    const auto title = reader.take_text(kTitleMaxBytes);
    const auto summary = reader.take_text(kSummaryMaxBytes);
    const auto owner = reader.take_text(kOwnerMaxBytes);
    const auto documentation = reader.take_text(kDocumentationMaxBytes);
    for (const auto* result : {&title, &summary, &owner, &documentation}) {
      if (!result->ok()) {
        return Result<ClassRevision>::failure(result->status());
      }
    }
    const auto assign = [](const Result<std::string>& source, std::size_t bound,
                           BoundedText& destination) -> Status {
      if (source.value().empty()) {
        return Status::success();
      }
      const auto created = BoundedText::create(source.value(), bound);
      if (!created.ok()) {
        return created.status();
      }
      destination = created.value();
      return Status::success();
    };
    Status assigned = assign(title, kTitleMaxBytes, revision.metadata.title);
    if (assigned.ok()) {
      assigned = assign(summary, kSummaryMaxBytes, revision.metadata.summary);
    }
    if (assigned.ok()) {
      assigned = assign(owner, kOwnerMaxBytes, revision.metadata.owner);
    }
    if (assigned.ok()) {
      assigned = assign(documentation, kDocumentationMaxBytes, revision.metadata.documentation);
    }
    if (!assigned.ok()) {
      return Result<ClassRevision>::failure(assigned);
    }
  }

  if (reader.remaining() != 0) {
    return Result<ClassRevision>::failure(
        ErrorCode::InvalidArgument,
        "revision payload has " + std::to_string(reader.remaining()) + " trailing bytes", "revision");
  }

  const Status validation = revision.validate();
  if (!validation.ok()) {
    return Result<ClassRevision>::failure(validation);
  }
  return Result<ClassRevision>::success(std::move(revision));
}

Result<std::vector<std::uint8_t>> encode_revision_frame(const ClassRevision& revision) {
  const auto payload = encode_revision_payload(revision);
  if (!payload.ok()) {
    return Result<std::vector<std::uint8_t>>::failure(payload.status());
  }
  return encode_frame(FrameKind::RevisionContent, payload.value(), kMaxRevisionPayloadBytes);
}

Result<ClassRevision> decode_revision_frame(const std::uint8_t* data, std::size_t size) {
  const auto frame = decode_frame(data, size, FrameKind::RevisionContent, kMaxRevisionPayloadBytes);
  if (!frame.ok()) {
    return Result<ClassRevision>::failure(frame.status());
  }
  return decode_revision_payload(frame.value().payload);
}

Result<Digest> revision_frame_digest(const ClassRevision& revision) {
  const auto frame = encode_revision_frame(revision);
  if (!frame.ok()) {
    return Result<Digest>::failure(frame.status());
  }
  return Result<Digest>::success(Digest::of(frame.value().data(), frame.value().size()));
}

Result<std::vector<std::uint8_t>> encode_guard_payload(const GuardRecord& guard) {
  if (guard.manifest_digest.is_null()) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::InvalidArgument,
                                                      "guard manifest digest must not be null", "guard");
  }
  ByteWriter writer;
  writer.put_u64(guard.sequence.value());
  writer.put_bytes(guard.manifest_digest.bytes().data(), guard.manifest_digest.bytes().size());
  writer.put_u64(guard.epoch.value());
  std::vector<std::uint8_t> payload = writer.take();
  if (payload.size() != kMaxGuardPayloadBytes) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::Internal,
                                                      "guard payload size is not the declared fixed size", "guard");
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(payload));
}

Result<GuardRecord> decode_guard_payload(const std::vector<std::uint8_t>& payload) {
  if (payload.size() != kMaxGuardPayloadBytes) {
    return Result<GuardRecord>::failure(
        ErrorCode::CorruptStore,
        "guard payload must be exactly " + std::to_string(kMaxGuardPayloadBytes) + " bytes, got " +
            std::to_string(payload.size()),
        "guard");
  }
  ByteReader reader(payload.data(), payload.size());
  const auto sequence = reader.take_u64();
  const auto digest = take_digest(reader);
  const auto epoch = reader.take_u64();
  if (!sequence.ok()) {
    return Result<GuardRecord>::failure(sequence.status());
  }
  if (!digest.ok()) {
    return Result<GuardRecord>::failure(digest.status());
  }
  if (!epoch.ok()) {
    return Result<GuardRecord>::failure(epoch.status());
  }
  if (digest.value().is_null()) {
    return Result<GuardRecord>::failure(ErrorCode::CorruptStore, "guard manifest digest is null", "guard");
  }
  GuardRecord guard;
  guard.sequence = Sequence::from(sequence.value());
  guard.manifest_digest = digest.value();
  guard.epoch = Epoch::from(epoch.value());
  return Result<GuardRecord>::success(guard);
}

}  // namespace scr
