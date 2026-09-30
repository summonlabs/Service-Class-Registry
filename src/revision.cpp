#include "scr/revision.hpp"

#include <algorithm>

namespace scr {
namespace {

Status invalid(std::string detail, std::string subject) {
  return Status::failure(ErrorCode::InvalidArgument, std::move(detail), std::move(subject));
}

}  // namespace

const char* to_string(ReferenceKind kind) noexcept {
  switch (kind) {
    case ReferenceKind::RequirementSet:
      return "requirement-set";
    case ReferenceKind::PolicyPredicate:
      return "policy-predicate";
    case ReferenceKind::EntitlementProfile:
      return "entitlement-profile";
    case ReferenceKind::EvidenceSource:
      return "evidence-source";
  }
  return "unknown";
}

Result<ReferenceKind> parse_reference_kind(std::string_view text) {
  if (text == "requirement-set") {
    return Result<ReferenceKind>::success(ReferenceKind::RequirementSet);
  }
  if (text == "policy-predicate") {
    return Result<ReferenceKind>::success(ReferenceKind::PolicyPredicate);
  }
  if (text == "entitlement-profile") {
    return Result<ReferenceKind>::success(ReferenceKind::EntitlementProfile);
  }
  if (text == "evidence-source") {
    return Result<ReferenceKind>::success(ReferenceKind::EvidenceSource);
  }
  return Result<ReferenceKind>::failure(ErrorCode::InvalidEnum, "unknown reference kind", "reference");
}

Result<ExternalReference> ExternalReference::create(ReferenceKind kind, std::string target, Digest digest,
                                                    AuthorityId authority, BoundedText note) {
  if (!is_valid_identifier_path(target, kReferenceTargetMaxSegments)) {
    return Result<ExternalReference>::failure(
        ErrorCode::InvalidIdentifier,
        "reference target must be a slash separated identifier path of at most 8 segments and 200 bytes", "reference");
  }
  if (!authority.empty()) {
    const auto parsed = AuthorityId::parse(authority.str());
    if (!parsed.ok()) {
      return Result<ExternalReference>::failure(parsed.status());
    }
  }
  ExternalReference reference;
  reference.kind = kind;
  reference.target = std::move(target);
  reference.digest = digest;
  reference.authority = std::move(authority);
  reference.note = std::move(note);
  return Result<ExternalReference>::success(std::move(reference));
}

std::string ExternalReference::to_string() const {
  std::string text = scr::to_string(kind);
  text += " ";
  text += target;
  text += " @";
  text += digest.is_null() ? std::string("unbound") : digest.hex();
  if (!authority.empty()) {
    text += " authority=";
    text += authority.str();
  }
  return text;
}

Result<CompositionRef> CompositionRef::create(ClassId parent, RevisionRef revision) {
  if (parent.empty()) {
    return Result<CompositionRef>::failure(ErrorCode::InvalidIdentifier,
                                           "composition parent identity is empty", "composition");
  }
  const auto parsed = ClassId::parse(parent.str());
  if (!parsed.ok()) {
    return Result<CompositionRef>::failure(parsed.status());
  }
  if (revision.generation.is_zero() || revision.revision.is_zero()) {
    return Result<CompositionRef>::failure(
        invalid("composition parent revision identity must have a non-zero generation and revision", "composition"));
  }
  if (revision.digest.is_null()) {
    return Result<CompositionRef>::failure(invalid(
        "composition must name the exact parent revision digest; a null digest is not a binding", "composition"));
  }
  CompositionRef reference;
  reference.parent = std::move(parent);
  reference.revision = revision;
  return Result<CompositionRef>::success(std::move(reference));
}

std::string CompositionRef::to_string() const {
  return parent.str() + " @ " + revision.to_string();
}

Result<Lineage> Lineage::from_predecessor(RevisionRef predecessor) {
  if (predecessor.generation.is_zero() || predecessor.revision.is_zero()) {
    return Result<Lineage>::failure(ErrorCode::InvalidArgument,
                                    "a predecessor must have a non-zero generation and revision", "lineage");
  }
  if (predecessor.digest.is_null()) {
    return Result<Lineage>::failure(ErrorCode::InvalidArgument,
                                    "a predecessor must carry its exact content digest", "lineage");
  }
  Lineage lineage;
  lineage.genesis_ = false;
  lineage.predecessor_ = predecessor;
  return Result<Lineage>::success(lineage);
}

bool reference_less(const ExternalReference& a, const ExternalReference& b) noexcept {
  if (a.kind != b.kind) {
    return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
  }
  if (a.target != b.target) {
    return a.target < b.target;
  }
  if (a.digest != b.digest) {
    return a.digest < b.digest;
  }
  return a.authority.str() < b.authority.str();
}

bool composition_less(const CompositionRef& a, const CompositionRef& b) noexcept {
  if (a.parent != b.parent) {
    return a.parent < b.parent;
  }
  return a.revision < b.revision;
}

Status canonicalize(ClassRevision& revision) {
  std::stable_sort(revision.references.begin(), revision.references.end(), reference_less);
  std::stable_sort(revision.composes.begin(), revision.composes.end(), composition_less);

  for (std::size_t index = 1; index < revision.references.size(); ++index) {
    const ExternalReference& previous = revision.references[index - 1];
    const ExternalReference& current = revision.references[index];
    if (previous.kind == current.kind && previous.target == current.target) {
      return Status::failure(ErrorCode::DuplicateIdentity,
                             std::string("reference '") + current.target +
                                 "' is declared more than once for the same kind",
                             current.target);
    }
  }

  for (std::size_t index = 1; index < revision.composes.size(); ++index) {
    if (revision.composes[index - 1].parent == revision.composes[index].parent) {
      return Status::failure(ErrorCode::DuplicateIdentity,
                             std::string("composition parent '") + revision.composes[index].parent.str() +
                                 "' is declared more than once",
                             revision.composes[index].parent.str());
    }
  }

  return Status::success();
}

Status ClassRevision::validate() const {
  if (class_id.empty()) {
    return Status::failure(ErrorCode::InvalidIdentifier, "class identity is empty", "class");
  }
  {
    const auto parsed = ClassId::parse(class_id.str());
    if (!parsed.ok()) {
      return parsed.status();
    }
  }
  if (generation.is_zero()) {
    return invalid("generation must be at least 1", "generation");
  }
  if (revision.is_zero()) {
    return invalid("revision must be at least 1", "revision");
  }

  if (!lineage.is_genesis()) {
    const RevisionRef& predecessor = lineage.predecessor();
    if (predecessor.generation.is_zero() || predecessor.revision.is_zero()) {
      return invalid("lineage predecessor must have a non-zero generation and revision", "lineage");
    }
    if (predecessor.digest.is_null()) {
      return invalid("lineage predecessor must carry its exact content digest", "lineage");
    }
    if (predecessor.generation == generation && predecessor.revision == revision) {
      return invalid("a revision cannot be its own predecessor", "lineage");
    }
  }

  if (obligations.empty()) {
    return Status::failure(ErrorCode::MissingRequirement,
                           "a service class must declare at least one typed obligation; an obligation-free class "
                           "would be an opaque label",
                           "obligations");
  }
  if (obligations.size() > kMaxObligationsPerRevision) {
    return Status::failure(ErrorCode::BoundExceeded,
                           "a revision may declare at most " + std::to_string(kMaxObligationsPerRevision) +
                               " obligations",
                           "obligations");
  }

  std::size_t required_count = 0;
  for (const auto& entry : obligations.items()) {
    const Obligation& obligation = entry.second;
    if (obligation.key() != entry.first) {
      return Status::failure(ErrorCode::Internal, "obligation set key does not match the obligation key",
                             describe(entry.first).name);
    }
    const auto validated = Obligation::create(obligation.key(), obligation.modality(), obligation.value());
    if (!validated.ok()) {
      return validated.status();
    }
    if (obligation.modality() == Modality::Required) {
      ++required_count;
    }
  }
  if (required_count == 0) {
    return Status::failure(ErrorCode::MissingRequirement,
                           "at least one obligation must be asserted as required", "obligations");
  }

  if (references.size() > kMaxReferencesPerRevision) {
    return Status::failure(ErrorCode::BoundExceeded,
                           "a revision may declare at most " + std::to_string(kMaxReferencesPerRevision) +
                               " external references",
                           "references");
  }
  for (std::size_t index = 0; index < references.size(); ++index) {
    const ExternalReference& reference = references[index];
    const auto validated =
        ExternalReference::create(reference.kind, reference.target, reference.digest, reference.authority,
                                  reference.note);
    if (!validated.ok()) {
      return validated.status();
    }
    if (index > 0 && reference_less(reference, references[index - 1])) {
      return invalid("external references are not in canonical order", "references");
    }
    if (index > 0 && references[index - 1].kind == reference.kind &&
        references[index - 1].target == reference.target) {
      return Status::failure(ErrorCode::DuplicateIdentity,
                             std::string("reference '") + reference.target + "' is declared more than once",
                             "references");
    }
  }

  if (composes.size() > kMaxCompositionsPerRevision) {
    return Status::failure(ErrorCode::BoundExceeded,
                           "a revision may compose at most " + std::to_string(kMaxCompositionsPerRevision) +
                               " parent revisions",
                           "composes");
  }
  for (std::size_t index = 0; index < composes.size(); ++index) {
    const CompositionRef& composition = composes[index];
    const auto validated = CompositionRef::create(composition.parent, composition.revision);
    if (!validated.ok()) {
      return validated.status();
    }
    if (index > 0 && composition_less(composition, composes[index - 1])) {
      return invalid("composition entries are not in canonical order", "composes");
    }
    if (index > 0 && composes[index - 1].parent == composition.parent) {
      return Status::failure(ErrorCode::DuplicateIdentity,
                             std::string("composition parent '") + composition.parent.str() +
                                 "' is declared more than once",
                             "composes");
    }
  }

  for (const BoundedText* text : {&metadata.title, &metadata.summary, &metadata.owner, &metadata.documentation}) {
    if (text->empty()) {
      continue;
    }
    if (!is_valid_utf8(text->value()) || has_control_characters(text->value())) {
      return Status::failure(ErrorCode::InvalidTextEncoding, "metadata text is not well-formed UTF-8", "metadata");
    }
  }
  if (metadata.title.value().size() > kTitleMaxBytes || metadata.summary.value().size() > kSummaryMaxBytes ||
      metadata.owner.value().size() > kOwnerMaxBytes ||
      metadata.documentation.value().size() > kDocumentationMaxBytes) {
    return Status::failure(ErrorCode::BoundExceeded, "metadata text exceeds its declared bound", "metadata");
  }

  return Status::success();
}

Result<ClassRevision> make_revision(ClassId class_id, Generation generation, Revision revision, Lineage lineage,
                                    ObligationSet obligations, std::vector<ExternalReference> references,
                                    std::vector<CompositionRef> composes, Metadata metadata) {
  ClassRevision content;
  content.class_id = std::move(class_id);
  content.generation = generation;
  content.revision = revision;
  content.lineage = std::move(lineage);
  content.obligations = std::move(obligations);
  content.references = std::move(references);
  content.composes = std::move(composes);
  content.metadata = std::move(metadata);

  const Status canonical = canonicalize(content);
  if (!canonical.ok()) {
    return Result<ClassRevision>::failure(canonical);
  }
  const Status validation = content.validate();
  if (!validation.ok()) {
    return Result<ClassRevision>::failure(validation);
  }
  return Result<ClassRevision>::success(std::move(content));
}

}  // namespace scr
