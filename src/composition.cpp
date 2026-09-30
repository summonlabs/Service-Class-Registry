#include "scr/composition.hpp"

#include <algorithm>
#include <set>
#include <tuple>

namespace scr {
namespace {

using NodeKey = std::tuple<std::string, std::uint64_t, std::uint32_t>;

NodeKey node_key(const ClassRevision& content) {
  return NodeKey(content.class_id.str(), content.generation.value(), content.revision.value());
}

std::string node_text(const ClassRevision& content) {
  return content.class_id.str() + " " + std::to_string(content.generation.value()) + "." +
         std::to_string(content.revision.value());
}

Diagnostic advisory(ErrorCode code, std::string subject, std::string detail) {
  Diagnostic diagnostic;
  diagnostic.code = code;
  diagnostic.severity = Severity::Advisory;
  diagnostic.subject = std::move(subject);
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

struct Flattener {
  const RevisionResolver& resolver;
  const FlattenOptions& options;
  std::map<NodeKey, FlattenedSemantics> memo;
  std::vector<NodeKey> path;
  std::vector<RevisionRef> resolution_order;
  std::size_t resolver_calls = 0;

  Result<FlattenedSemantics> run(const ClassRevision& content, const Digest& digest, std::size_t depth);

  Status refuse(ErrorCode code, std::string detail, std::string subject) const {
    return Status::failure(code, std::move(detail), std::move(subject));
  }
};

Provenance make_provenance(const ClassRevision& owner, const Digest& digest, const Obligation& obligation,
                           bool direct) {
  Provenance provenance;
  provenance.class_id = owner.class_id;
  provenance.revision.generation = owner.generation;
  provenance.revision.revision = owner.revision;
  provenance.revision.digest = digest;
  provenance.modality = obligation.modality();
  provenance.value = obligation.value();
  provenance.direct = direct;
  return provenance;
}

void push_suppressed_from(const EffectiveObligation& loser, std::vector<Suppression>& target,
                          const std::string& reason) {
  for (const Provenance& provenance : loser.provenance) {
    Suppression suppression;
    suppression.class_id = provenance.class_id;
    suppression.revision = provenance.revision;
    suppression.modality = provenance.modality;
    suppression.value = provenance.value;
    suppression.reason = reason;
    target.push_back(std::move(suppression));
  }
}

std::string describe_value(ObligationKey key, Modality modality, std::uint64_t value) {
  const KeyDescriptor& descriptor = describe(key);
  std::string text = descriptor.name;
  text += "=";
  if (descriptor.kind == ValueKind::Enumerated) {
    for (std::size_t index = 0; index < descriptor.enum_count; ++index) {
      if (descriptor.enum_names[index].value == value) {
        text += descriptor.enum_names[index].name;
        break;
      }
    }
  } else {
    text += std::to_string(value);
  }
  text += " (";
  text += to_string(modality);
  text += ")";
  return text;
}

}  // namespace

Result<FlattenedSemantics> Flattener::run(const ClassRevision& content, const Digest& digest, std::size_t depth) {
  const NodeKey key = node_key(content);
  if (const auto found = memo.find(key); found != memo.end()) {
    return Result<FlattenedSemantics>::success(found->second);
  }
  if (std::find(path.begin(), path.end(), key) != path.end()) {
    std::string cycle;
    bool started = false;
    for (const NodeKey& node : path) {
      if (node == key) {
        started = true;
      }
      if (started) {
        cycle += std::get<0>(node) + " " + std::to_string(std::get<1>(node)) + "." +
                 std::to_string(std::get<2>(node)) + " -> ";
      }
    }
    cycle += content.class_id.str() + " " + std::to_string(content.generation.value()) + "." +
             std::to_string(content.revision.value());
    return Result<FlattenedSemantics>::failure(
        refuse(ErrorCode::CycleDetected, "composition cycle detected: " + cycle, content.class_id.str()));
  }
  if (depth > options.max_depth) {
    return Result<FlattenedSemantics>::failure(refuse(
        ErrorCode::BoundExceeded,
        "composition depth exceeds the limit of " + std::to_string(options.max_depth) + " at " + node_text(content),
        content.class_id.str()));
  }

  path.push_back(key);

  FlattenedSemantics result;
  std::map<ObligationKey, EffectiveObligation> accumulator;
  std::vector<Diagnostic> advisories;

  for (const CompositionRef& parent : content.composes) {
    if (parent.parent == content.class_id && parent.revision.generation == content.generation &&
        parent.revision.revision == content.revision) {
      path.pop_back();
      return Result<FlattenedSemantics>::failure(
          refuse(ErrorCode::CycleDetected,
                 "revision " + node_text(content) + " composes itself", content.class_id.str()));
    }
    // A node that has already been flattened is reused without calling the
    // resolver again, so a diamond composes its shared ancestor exactly once.
    // Retirement was already decided on the first visit of the node, and the
    // publication/historical choice is constant for one flatten call.
    FlattenedSemantics parent_semantics;
    const NodeKey parent_key(parent.parent.str(), parent.revision.generation.value(),
                             parent.revision.revision.value());
    const auto memo_found = memo.find(parent_key);
    if (memo_found != memo.end()) {
      parent_semantics = memo_found->second;
    } else {
      if (resolver_calls >= options.max_revisions) {
        path.pop_back();
        return Result<FlattenedSemantics>::failure(refuse(
            ErrorCode::BoundExceeded,
            "composition resolution exceeded the limit of " + std::to_string(options.max_revisions) +
                " revisions",
            content.class_id.str()));
      }
      const Result<ResolvedRevision> resolved = resolver(parent.revision);
      ++resolver_calls;
      if (!resolved.ok()) {
        path.pop_back();
        Status status = resolved.status();
        status.add_secondary(Diagnostic{status.code(), Severity::Refusal, content.class_id.str(),
                                        "while resolving composition parent " + parent.parent.str()});
        return Result<FlattenedSemantics>::failure(status);
      }
      const ClassRevision& parent_content = resolved.value().content;
      if (parent_content.class_id != parent.parent || parent_content.generation != parent.revision.generation ||
          parent_content.revision != parent.revision.revision) {
        path.pop_back();
        return Result<FlattenedSemantics>::failure(refuse(
            ErrorCode::DigestMismatch,
            "resolver returned " + node_text(parent_content) + " for composition parent " + parent.parent.str() + " " +
                parent.revision.to_display_string(),
            content.class_id.str()));
      }
      if (resolved.value().retired && !options.allow_retired_parents) {
        path.pop_back();
        return Result<FlattenedSemantics>::failure(refuse(
            ErrorCode::RetiredClass,
            "composition parent class '" + parent.parent.str() +
                "' is retired and cannot be composed by a new revision",
            content.class_id.str()));
      }
      if (resolved.value().retired) {
        advisories.push_back(advisory(ErrorCode::HistoricalRetiredParent, parent.parent.str(),
                                      "composition parent revision " + parent.revision.to_display_string() +
                                          " belongs to a retired class; its obligations remain authoritative for "
                                          "historical decisions"));
      }
      auto parent_result = run(parent_content, parent.revision.digest, depth + 1);
      if (!parent_result.ok()) {
        path.pop_back();
        return Result<FlattenedSemantics>::failure(parent_result.status());
      }
      parent_semantics = parent_result.take();
    }

    for (const Diagnostic& diagnostic : parent_semantics.advisories) {
      advisories.push_back(diagnostic);
    }
    for (const EffectiveObligation& incoming : parent_semantics.obligations) {
      EffectiveObligation copy = incoming;
      copy.inherited = true;
      for (Provenance& provenance : copy.provenance) {
        provenance.direct = false;
      }
      const auto found = accumulator.find(copy.key);
      if (found == accumulator.end()) {
        accumulator.emplace(copy.key, std::move(copy));
        continue;
      }
      EffectiveObligation& current = found->second;
      const MergeOrder order = compare_modality(current.modality, copy.modality);
      if (order == MergeOrder::LeftDominates) {
        push_suppressed_from(copy, current.suppressed, "dominated-by-modality");
        advisories.push_back(advisory(ErrorCode::DominatedByModality, describe(copy.key).name,
                                      describe_value(copy.key, copy.modality, copy.value) +
                                          " inherited from " + copy.provenance.front().class_id.str() +
                                          " is dominated by " +
                                          describe_value(current.key, current.modality, current.value)));
        continue;
      }
      if (order == MergeOrder::RightDominates) {
        push_suppressed_from(current, copy.suppressed, "dominated-by-modality");
        advisories.push_back(advisory(ErrorCode::DominatedByModality, describe(copy.key).name,
                                      describe_value(current.key, current.modality, current.value) +
                                          " is dominated by " +
                                          describe_value(copy.key, copy.modality, copy.value) + " inherited from " +
                                          copy.provenance.front().class_id.str()));
        accumulator[copy.key] = std::move(copy);
        continue;
      }
      if (current.value == copy.value) {
        for (const Provenance& provenance : copy.provenance) {
          current.provenance.push_back(provenance);
        }
        for (const Suppression& suppression : copy.suppressed) {
          current.suppressed.push_back(suppression);
        }
        continue;
      }
      const auto relation = compare_strictness(current.key, current.value, copy.value);
      if (relation == StrictnessComparison::Relation::Equal) {
        for (const Provenance& provenance : copy.provenance) {
          current.provenance.push_back(provenance);
        }
        continue;
      }
      if (relation == StrictnessComparison::Relation::Incomparable) {
        path.pop_back();
        return Result<FlattenedSemantics>::failure(refuse(
            ErrorCode::ContradictoryObligations,
            "inherited obligations disagree on '" + std::string(describe(current.key).name) + "': " +
                describe_value(current.key, current.modality, current.value) + " and " +
                describe_value(copy.key, copy.modality, copy.value) +
                " are different values of an incomparable key; composition is refused rather than resolved "
                "silently",
            describe(current.key).name));
      }
      if (relation == StrictnessComparison::Relation::AStricter) {
        push_suppressed_from(copy, current.suppressed, "dominated-by-strictness");
        advisories.push_back(advisory(ErrorCode::MergedStricter, describe(current.key).name,
                                      describe_value(copy.key, copy.modality, copy.value) + " inherited from " +
                                          copy.provenance.front().class_id.str() + " is dominated by the stricter " +
                                          describe_value(current.key, current.modality, current.value)));
      } else {
        push_suppressed_from(current, copy.suppressed, "dominated-by-strictness");
        advisories.push_back(advisory(ErrorCode::MergedStricter, describe(current.key).name,
                                      describe_value(current.key, current.modality, current.value) +
                                          " is dominated by the stricter " +
                                          describe_value(copy.key, copy.modality, copy.value) + " inherited from " +
                                          copy.provenance.front().class_id.str()));
        accumulator[copy.key] = std::move(copy);
      }
    }
  }

  // Own declarations: a target may strengthen inherited requirements but never
  // weaken them, and may never override an explicitly asserted requirement with
  // "explicitly unspecified".
  for (const auto& entry : content.obligations.items()) {
    const Obligation& own = entry.second;
    const auto found = accumulator.find(entry.first);
    if (found == accumulator.end()) {
      EffectiveObligation effective;
      effective.key = own.key();
      effective.modality = own.modality();
      effective.value = own.value();
      effective.inherited = false;
      effective.provenance.push_back(make_provenance(content, digest, own, true));
      accumulator.emplace(entry.first, std::move(effective));
      continue;
    }

    EffectiveObligation& inherited = found->second;
    if (own.modality() == Modality::Unspecified) {
      if (inherited.modality == Modality::Required) {
        path.pop_back();
        return Result<FlattenedSemantics>::failure(refuse(
            ErrorCode::WeakenedRequirement,
            "the target revision declares '" + std::string(describe(entry.first).name) +
                "' as explicitly unspecified, but an inherited obligation is required; a required obligation "
                "cannot be replaced by an unspecified declaration",
            describe(entry.first).name));
      }
      push_suppressed_from(
          EffectiveObligation{entry.first, own.modality(), own.value(), false,
                              {make_provenance(content, digest, own, true)}, {}},
          inherited.suppressed, "explicit-unspecified-does-not-override");
      advisories.push_back(advisory(ErrorCode::ExplicitUnspecifiedOverridden, describe(entry.first).name,
                                    "the target revision declares '" + std::string(describe(entry.first).name) +
                                        "' as explicitly unspecified, which does not override the inherited " +
                                        describe_value(entry.first, inherited.modality, inherited.value)));
      continue;
    }

    if (inherited.modality == Modality::Required) {
      if (own.modality() != Modality::Required) {
        path.pop_back();
        return Result<FlattenedSemantics>::failure(refuse(
            ErrorCode::WeakenedRequirement,
            "the target revision declares '" + std::string(describe(entry.first).name) + "' as " +
                to_string(own.modality()) + ", but the inherited obligation is required",
            describe(entry.first).name));
      }
      const auto relation = compare_strictness(entry.first, own.value(), inherited.value);
      if (relation == StrictnessComparison::Relation::Incomparable && own.value() != inherited.value) {
        path.pop_back();
        return Result<FlattenedSemantics>::failure(refuse(
            ErrorCode::ContradictoryObligations,
            "the target revision and an inherited required obligation disagree on '" +
                std::string(describe(entry.first).name) + "': " + describe_value(entry.first, own.modality(), own.value()) +
                " against " + describe_value(entry.first, inherited.modality, inherited.value),
            describe(entry.first).name));
      }
      if (relation == StrictnessComparison::Relation::BStricter) {
        path.pop_back();
        return Result<FlattenedSemantics>::failure(refuse(
            ErrorCode::WeakenedRequirement,
            "the target revision weakens the inherited required obligation '" +
                std::string(describe(entry.first).name) + "': " + describe_value(entry.first, own.modality(), own.value()) +
                " is weaker than " + describe_value(entry.first, inherited.modality, inherited.value),
            describe(entry.first).name));
      }
      if (relation == StrictnessComparison::Relation::AStricter) {
        EffectiveObligation strengthened;
        strengthened.key = own.key();
        strengthened.modality = own.modality();
        strengthened.value = own.value();
        strengthened.inherited = false;
        strengthened.provenance.push_back(make_provenance(content, digest, own, true));
        push_suppressed_from(inherited, strengthened.suppressed, "strengthened-by-target");
        advisories.push_back(advisory(ErrorCode::StrengthenedInheritance, describe(entry.first).name,
                                      "the target revision strengthens '" + std::string(describe(entry.first).name) +
                                          "' from " + describe_value(entry.first, inherited.modality, inherited.value) +
                                          " to " + describe_value(entry.first, own.modality(), own.value())));
        accumulator[entry.first] = std::move(strengthened);
        continue;
      }
      inherited.provenance.push_back(make_provenance(content, digest, own, true));
      continue;
    }

    const MergeOrder order = compare_modality(own.modality(), inherited.modality);
    if (order == MergeOrder::LeftDominates) {
      EffectiveObligation dominated;
      dominated.key = own.key();
      dominated.modality = own.modality();
      dominated.value = own.value();
      dominated.inherited = false;
      dominated.provenance.push_back(make_provenance(content, digest, own, true));
      push_suppressed_from(inherited, dominated.suppressed, "dominated-by-modality");
      advisories.push_back(advisory(ErrorCode::DominatedByModality, describe(entry.first).name,
                                    "the target revision asserts " + describe_value(entry.first, own.modality(), own.value()) +
                                        " over the inherited " +
                                        describe_value(entry.first, inherited.modality, inherited.value)));
      accumulator[entry.first] = std::move(dominated);
      continue;
    }
    if (order == MergeOrder::RightDominates) {
      push_suppressed_from(EffectiveObligation{entry.first, own.modality(), own.value(), false,
                                               {make_provenance(content, digest, own, true)}, {}},
                           inherited.suppressed, "dominated-by-modality");
      advisories.push_back(advisory(ErrorCode::DominatedByModality, describe(entry.first).name,
                                    describe_value(entry.first, own.modality(), own.value()) +
                                        " declared by the target revision is dominated by the inherited " +
                                        describe_value(entry.first, inherited.modality, inherited.value)));
      continue;
    }
    if (own.value() == inherited.value) {
      inherited.provenance.push_back(make_provenance(content, digest, own, true));
      continue;
    }
    const auto relation = compare_strictness(entry.first, own.value(), inherited.value);
    if (relation == StrictnessComparison::Relation::Incomparable) {
      path.pop_back();
      return Result<FlattenedSemantics>::failure(refuse(
          ErrorCode::ContradictoryObligations,
          "the target revision and an inherited obligation disagree on '" +
              std::string(describe(entry.first).name) + "': " + describe_value(entry.first, own.modality(), own.value()) +
              " against " + describe_value(entry.first, inherited.modality, inherited.value),
          describe(entry.first).name));
    }
    if (relation == StrictnessComparison::Relation::AStricter) {
      EffectiveObligation dominated;
      dominated.key = own.key();
      dominated.modality = own.modality();
      dominated.value = own.value();
      dominated.inherited = false;
      dominated.provenance.push_back(make_provenance(content, digest, own, true));
      push_suppressed_from(inherited, dominated.suppressed, "dominated-by-strictness");
      advisories.push_back(advisory(ErrorCode::MergedStricter, describe(entry.first).name,
                                    "the target revision asserts the stricter " +
                                        describe_value(entry.first, own.modality(), own.value()) + " over " +
                                        describe_value(entry.first, inherited.modality, inherited.value)));
      accumulator[entry.first] = std::move(dominated);
      continue;
    }
    push_suppressed_from(EffectiveObligation{entry.first, own.modality(), own.value(), false,
                                             {make_provenance(content, digest, own, true)}, {}},
                         inherited.suppressed, "dominated-by-strictness");
    advisories.push_back(advisory(ErrorCode::MergedStricter, describe(entry.first).name,
                                  describe_value(entry.first, own.modality(), own.value()) +
                                      " declared by the target revision is dominated by the stricter inherited " +
                                      describe_value(entry.first, inherited.modality, inherited.value)));
  }

  path.pop_back();

  for (auto& entry : accumulator) {
    EffectiveObligation& effective = entry.second;
    std::sort(effective.provenance.begin(), effective.provenance.end(),
              [](const Provenance& a, const Provenance& b) {
                if (a.class_id != b.class_id) {
                  return a.class_id < b.class_id;
                }
                if (a.revision != b.revision) {
                  return a.revision < b.revision;
                }
                if (a.modality != b.modality) {
                  return static_cast<std::uint8_t>(a.modality) < static_cast<std::uint8_t>(b.modality);
                }
                return a.value < b.value;
              });
    std::sort(effective.suppressed.begin(), effective.suppressed.end(),
              [](const Suppression& a, const Suppression& b) {
                if (a.class_id != b.class_id) {
                  return a.class_id < b.class_id;
                }
                if (a.revision != b.revision) {
                  return a.revision < b.revision;
                }
                if (a.reason != b.reason) {
                  return a.reason < b.reason;
                }
                return a.value < b.value;
              });
    effective.inherited = !(effective.provenance.size() == 1 && effective.provenance.front().direct);
    result.obligations.push_back(effective);
  }

  std::sort(advisories.begin(), advisories.end(), [](const Diagnostic& a, const Diagnostic& b) {
    if (a.subject != b.subject) {
      return a.subject < b.subject;
    }
    if (a.detail != b.detail) {
      return a.detail < b.detail;
    }
    return static_cast<std::uint16_t>(a.code) < static_cast<std::uint16_t>(b.code);
  });

  result.advisories = std::move(advisories);
  result.resolution_order = resolution_order;
  result.resolved_revisions = resolver_calls;
  resolution_order.push_back(RevisionRef{content.generation, content.revision, digest});
  memo.emplace(key, result);
  return Result<FlattenedSemantics>::success(std::move(result));
}

Result<FlattenedSemantics> flatten_revision(const ClassRevision& content, const RevisionRef& identity,
                                            const RevisionResolver& resolver, const FlattenOptions& options) {
  const Status validation = content.validate();
  if (!validation.ok()) {
    return Result<FlattenedSemantics>::failure(validation);
  }
  if (identity.generation != content.generation || identity.revision != content.revision) {
    return Result<FlattenedSemantics>::failure(
        Status::failure(ErrorCode::InvalidArgument,
                        "the supplied identity does not match the revision being flattened", content.class_id.str()));
  }
  if (!resolver) {
    return Result<FlattenedSemantics>::failure(
        ErrorCode::InvalidArgument, "composition resolver is not set", content.class_id.str());
  }
  Flattener flattener{resolver, options, {}, {}, {}, 0};
  return flattener.run(content, identity.digest, 0);
}

Result<FlattenedSemantics> flatten_revision(const ClassRevision& content, const RevisionResolver& resolver,
                                            const FlattenOptions& options) {
  RevisionRef identity;
  identity.generation = content.generation;
  identity.revision = content.revision;
  return flatten_revision(content, identity, resolver, options);
}

Result<EffectiveObligation> find_effective(const FlattenedSemantics& semantics, ObligationKey key) {
  for (const EffectiveObligation& obligation : semantics.obligations) {
    if (obligation.key == key) {
      return Result<EffectiveObligation>::success(obligation);
    }
  }
  return Result<EffectiveObligation>::failure(ErrorCode::UnknownField,
                                              std::string("obligation '") + describe(key).name +
                                                  "' is not part of the effective semantics",
                                              describe(key).name);
}

ObligationSet effective_obligation_set(const FlattenedSemantics& semantics) {
  ObligationSet set;
  for (const EffectiveObligation& effective : semantics.obligations) {
    const auto obligation = Obligation::create(effective.key, effective.modality, effective.value);
    if (obligation.ok()) {
      static_cast<void>(set.add(obligation.value()));
    }
  }
  return set;
}

}  // namespace scr
