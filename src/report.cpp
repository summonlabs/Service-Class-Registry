#include "scr/report.hpp"

#include <cstdio>

namespace scr {
namespace {

std::string quote(const std::string& text) {
  return "\"" + json_escape(text) + "\"";
}

std::string number(std::uint64_t value) {
  return std::to_string(value);
}

std::string key_name(ObligationKey key) {
  return describe(key).name;
}

std::string modality_name(Modality modality) {
  return to_string(modality);
}

std::string value_text(ObligationKey key, Modality modality, std::uint64_t value) {
  const auto obligation = Obligation::create(key, modality, value);
  if (obligation.ok()) {
    return obligation.value().value_text();
  }
  return std::to_string(value);
}

std::string revision_json(const RevisionRef& reference) {
  return "{\"generation\":" + number(reference.generation.value()) + ",\"revision\":" +
         number(reference.revision.value()) + ",\"digest\":" + quote(reference.digest.hex()) + "}";
}

std::string timestamp_json(const Timestamp& timestamp) {
  if (!timestamp.has_value()) {
    return "null";
  }
  return "{\"unix_seconds\":" + std::to_string(timestamp.unix_seconds()) + ",\"iso8601\":" +
         quote(timestamp.to_iso8601()) + "}";
}

std::string diagnostics_json(const std::vector<Diagnostic>& diagnostics) {
  std::string out = "[";
  for (std::size_t index = 0; index < diagnostics.size(); ++index) {
    const Diagnostic& diagnostic = diagnostics[index];
    if (index != 0) {
      out += ",";
    }
    out += "{\"severity\":" + quote(to_string(diagnostic.severity)) +
           ",\"code\":" + quote(to_string(diagnostic.code)) + ",\"subject\":" + quote(diagnostic.subject) +
           ",\"detail\":" + quote(diagnostic.detail) + "}";
  }
  out += "]";
  return out;
}

std::string diagnostics_text(const std::vector<Diagnostic>& diagnostics) {
  std::string out;
  for (const Diagnostic& diagnostic : diagnostics) {
    out += "  [";
    out += to_string(diagnostic.severity);
    out += "] ";
    out += to_string(diagnostic.code);
    if (!diagnostic.subject.empty()) {
      out += " ";
      out += diagnostic.subject;
    }
    out += ": ";
    out += diagnostic.detail;
    out += "\n";
  }
  return out;
}

std::string history_json(const std::vector<ClassHistoryEntry>& history) {
  std::string out = "[";
  for (std::size_t index = 0; index < history.size(); ++index) {
    const ClassHistoryEntry& entry = history[index];
    if (index != 0) {
      out += ",";
    }
    out += "{\"reference\":" + revision_json(entry.reference) +
           ",\"published_at\":" + timestamp_json(entry.published_at) +
           ",\"generation_start\":" + (entry.generation_start ? "true" : "false") + "}";
  }
  out += "]";
  return out;
}

std::string class_record_json(const ClassRecord& record) {
  std::string out = "{";
  out += "\"class\":" + quote(record.class_id.str());
  out += ",\"state\":" + quote(to_string(record.state));
  out += ",\"generation\":" + number(record.generation.value());
  out += ",\"tip\":" + number(record.tip.value());
  out += ",\"tip_digest\":" + quote(record.tip_digest.hex());
  out += ",\"published_at\":" + timestamp_json(record.published_at);
  out += ",\"retired_at\":" + timestamp_json(record.retired_at);
  out += ",\"retire_reason\":" + quote(record.retire_reason.value());
  out += ",\"history\":" + history_json(record.history);
  out += "}";
  return out;
}

}  // namespace

std::string json_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (byte < 0x20u) {
          static const char* kDigits = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kDigits[(byte >> 4) & 0x0F]);
          out.push_back(kDigits[byte & 0x0F]);
        } else {
          out.push_back(character);
        }
        break;
    }
  }
  return out;
}

std::string render_text(const PublishOutcome& outcome) {
  std::string out = "published ";
  out += outcome.reference.to_string();
  out += outcome.idempotent ? " (idempotent replay)\n" : "\n";
  out += diagnostics_text(outcome.advisories);
  return out;
}

std::string render_text(const RetireOutcome& outcome) {
  std::string out = "retired ";
  out += outcome.class_id.str();
  out += " at tip ";
  out += outcome.tip.to_string();
  out += " (";
  out += outcome.retired_at.to_iso8601();
  out += ")";
  out += outcome.already_retired ? " [already retired]\n" : "\n";
  return out;
}

std::string render_text(const BindingResolution& resolution) {
  std::string out = std::string("state: ") + to_string(resolution.state) + "\n";
  out += "binding: " + resolution.binding.to_string() + "\n";
  if (resolution.tip_present) {
    out += "authoritative tip: " + resolution.authoritative_tip.to_string() + "\n";
  }
  out += "detail: " + resolution.detail + "\n";
  return out;
}

std::string render_text(const ClassRecord& record) {
  std::string out = "class: " + record.class_id.str() + "\n";
  out += std::string("state: ") + to_string(record.state) + "\n";
  out += "generation: " + std::to_string(record.generation.value()) + "\n";
  out += "tip: " + std::to_string(record.tip.value()) + "\n";
  out += "tip digest: " + record.tip_digest.hex() + "\n";
  out += "published at: " + record.published_at.to_iso8601() + "\n";
  if (record.state == ClassState::Retired) {
    out += "retired at: " + record.retired_at.to_iso8601() + "\n";
    out += "retire reason: " + record.retire_reason.value() + "\n";
  }
  out += "history:\n";
  for (const ClassHistoryEntry& entry : record.history) {
    out += "  " + entry.reference.to_string() + "  " + entry.published_at.to_iso8601();
    if (entry.generation_start) {
      out += "  [generation start]";
    }
    out += "\n";
  }
  return out;
}

std::string render_text(const CompareReport& report) {
  std::string out = "class: " + report.class_id.str() + "\n";
  out += "from: " + report.from.to_string() + "\n";
  out += "to:   " + report.to.to_string() + "\n";
  out += std::string("relation: ") + to_string(report.relation) + "\n";
  out += std::string("identical content: ") + (report.identical_content ? "yes" : "no") + "\n";
  out += "declared obligation changes:\n";
  for (const ObligationDelta& delta : report.declared) {
    out += "  " + key_name(delta.key) + ": ";
    if (!delta.from_present) {
      out += "(absent)";
    } else {
      out += value_text(delta.key, delta.from_modality, delta.from_value) + " (" + modality_name(delta.from_modality) + ")";
    }
    out += " -> ";
    if (!delta.to_present) {
      out += "(absent)";
    } else {
      out += value_text(delta.key, delta.to_modality, delta.to_value) + " (" + modality_name(delta.to_modality) + ")";
    }
    out += std::string("  [") + delta.strictness_effect + "]\n";
  }
  out += "effective obligation changes:\n";
  for (const ObligationDelta& delta : report.effective) {
    out += "  " + key_name(delta.key) + ": ";
    if (!delta.from_present) {
      out += "(absent)";
    } else {
      out += value_text(delta.key, delta.from_modality, delta.from_value) + " (" + modality_name(delta.from_modality) + ")";
    }
    out += " -> ";
    if (!delta.to_present) {
      out += "(absent)";
    } else {
      out += value_text(delta.key, delta.to_modality, delta.to_value) + " (" + modality_name(delta.to_modality) + ")";
    }
    out += std::string("  [") + delta.strictness_effect + "]\n";
  }
  if (!report.reference_changes.empty()) {
    out += "reference changes:\n";
    for (const std::string& change : report.reference_changes) {
      out += "  " + change + "\n";
    }
  }
  if (!report.composition_changes.empty()) {
    out += "composition changes:\n";
    for (const std::string& change : report.composition_changes) {
      out += "  " + change + "\n";
    }
  }
  if (!report.metadata_changes.empty()) {
    out += "metadata changes:\n";
    for (const std::string& change : report.metadata_changes) {
      out += "  " + change + "\n";
    }
  }
  return out;
}

std::string render_text(const ClassExplanation& explanation) {
  std::string out = "class: " + explanation.class_id.str() + "\n";
  out += std::string("state: ") + to_string(explanation.state) + "\n";
  if (explanation.tip_present) {
    out += "authoritative revision: " + explanation.authoritative.to_string() + "\n";
  }
  out += "published at: " + explanation.published_at.to_iso8601() + "\n";
  if (explanation.state == ClassState::Retired) {
    out += "retired at: " + explanation.retired_at.to_iso8601() + "\n";
    out += "retire reason: " + explanation.retire_reason + "\n";
  }
  out += "effective obligations:\n";
  for (const ObligationExplanation& obligation : explanation.obligations) {
    out += "  " + key_name(obligation.key) + " = " +
           value_text(obligation.key, obligation.modality, obligation.value) + " (" +
           modality_name(obligation.modality) + ")" + (obligation.inherited ? " [inherited]" : " [declared]") + "\n";
    for (const Provenance& provenance : obligation.provenance) {
      out += "      from " + provenance.class_id.str() + " " +
             std::to_string(provenance.revision.generation.value()) + "." +
             std::to_string(provenance.revision.revision.value()) + "@" +
             provenance.revision.digest.short_hex() + " " +
             value_text(obligation.key, provenance.modality, provenance.value) +
             (provenance.direct ? " (direct)" : " (inherited)") + "\n";
    }
    for (const Suppression& suppression : obligation.suppressed) {
      out += "      suppressed " + suppression.reason + ": " + suppression.class_id.str() + " " +
             std::to_string(suppression.revision.generation.value()) + "." +
             std::to_string(suppression.revision.revision.value()) + "@" +
             suppression.revision.digest.short_hex() + " " +
             value_text(obligation.key, suppression.modality, suppression.value) + "\n";
    }
  }
  if (!explanation.composes.empty()) {
    out += "composition:\n";
    for (const CompositionRef& composition : explanation.composes) {
      out += "  " + composition.to_string() + "\n";
    }
  }
  if (!explanation.references.empty()) {
    out += "references:\n";
    for (const ExternalReference& reference : explanation.references) {
      out += "  " + reference.to_string() + "\n";
    }
  }
  if (!explanation.rules.empty()) {
    out += "rule evaluation:\n";
    for (const RuleViolation& violation : explanation.rules) {
      out += std::string("  ") + violation.rule + " [" + to_string(violation.severity) + "] " + violation.detail + "\n";
    }
  }
  if (!explanation.advisories.empty()) {
    out += "advisories:\n" + diagnostics_text(explanation.advisories);
  }
  if (!explanation.resolution_order.empty()) {
    out += "resolution order (parents first):\n";
    for (const RevisionRef& reference : explanation.resolution_order) {
      out += "  " + reference.to_display_string() + "\n";
    }
  }
  return out;
}

std::string render_text(const StoreInfo& info) {
  std::string out = "store root: " + info.root.string() + "\n";
  out += std::string("mode: ") + to_string(info.mode) + "\n";
  out += "authority: " + info.authority.str() + "\n";
  out += "control epoch: " + std::to_string(info.epoch.value()) + "\n";
  out += "manifest sequence: " + std::to_string(info.sequence.value()) + "\n";
  out += "manifest digest: " + info.manifest_digest.hex() + "\n";
  out += "index digest: " + info.index_digest.hex() + "\n";
  out += "written at: " + info.written_at.to_iso8601() + "\n";
  out += "classes: " + std::to_string(info.class_count) + "\n";
  out += "revisions: " + std::to_string(info.revision_count) + "\n";
  out += "revision records: " + std::to_string(info.record_files) + "\n";
  out += "unreferenced revision records: " + std::to_string(info.orphan_records) + "\n";
  out += "staging files removed at open: " + std::to_string(info.staging_files_removed) + "\n";
  out += std::string("guard fence repaired at open: ") + (info.guard_repaired ? "yes" : "no") + "\n";
  out += std::string("exclusive writer lock held: ") + (info.lock_held_exclusive ? "yes" : "no") + "\n";
  return out;
}

std::string render_text(const CommitOutcome& outcome) {
  std::string out = "commit sequence: " + std::to_string(outcome.sequence.value()) + "\n";
  out += "manifest digest: " + outcome.manifest_digest.hex() + "\n";
  out += "revision records written: " + std::to_string(outcome.records_written) + "\n";
  out += outcome.idempotent ? "idempotent: the index was already committed\n" : "";
  return out;
}

std::string render_text(const StoreVerifyReport& report) {
  std::string out = std::string("verified: ") + (report.ok ? "yes" : "no") + "\n";
  out += "manifest sequence: " + std::to_string(report.sequence.value()) + "\n";
  out += "manifest digest: " + report.manifest_digest.hex() + "\n";
  out += "referenced records: " + std::to_string(report.referenced_records) + "\n";
  out += "verified records: " + std::to_string(report.verified_records) + "\n";
  out += "unreferenced records: " + std::to_string(report.orphan_records) + "\n";
  out += "staging files: " + std::to_string(report.staging_files) + "\n";
  if (!report.problems.empty()) {
    out += "problems:\n";
    for (const std::string& problem : report.problems) {
      out += "  " + problem + "\n";
    }
  }
  return out;
}

std::string render_text(const StorePublishOutcome& outcome) {
  return render_text(outcome.publish) + render_text(outcome.commit);
}

std::string render_text(const StoreRetireOutcome& outcome) {
  return render_text(outcome.retire) + render_text(outcome.commit);
}

std::string render_json(const PublishOutcome& outcome) {
  std::string out = "{\"reference\":" + revision_json(outcome.reference) +
                    ",\"idempotent\":" + (outcome.idempotent ? "true" : "false") +
                    ",\"advisories\":" + diagnostics_json(outcome.advisories) + "}";
  return out;
}

std::string render_json(const RetireOutcome& outcome) {
  return "{\"class\":" + quote(outcome.class_id.str()) + ",\"tip\":" + revision_json(outcome.tip) +
         ",\"retired_at\":" + timestamp_json(outcome.retired_at) +
         ",\"already_retired\":" + (outcome.already_retired ? "true" : "false") + "}";
}

std::string render_json(const BindingResolution& resolution) {
  std::string out = "{\"state\":" + quote(to_string(resolution.state));
  out += ",\"binding\":{\"class\":" + quote(resolution.binding.class_id.str()) +
         ",\"revision\":" + revision_json(resolution.binding.revision) + "}";
  out += ",\"tip_present\":" + std::string(resolution.tip_present ? "true" : "false");
  if (resolution.tip_present) {
    out += ",\"authoritative_tip\":" + revision_json(resolution.authoritative_tip);
  }
  out += ",\"detail\":" + quote(resolution.detail) + "}";
  return out;
}

std::string render_json(const ClassRecord& record) {
  return class_record_json(record);
}

std::string render_json(const CompareReport& report) {
  const auto deltas_json = [](const std::vector<ObligationDelta>& deltas) {
    std::string out = "[";
    for (std::size_t index = 0; index < deltas.size(); ++index) {
      const ObligationDelta& delta = deltas[index];
      if (index != 0) {
        out += ",";
      }
      out += "{\"key\":" + quote(key_name(delta.key)) +
             ",\"from_present\":" + (delta.from_present ? "true" : "false") +
             ",\"from_value\":" + (delta.from_present ? number(delta.from_value) : std::string("null")) +
             ",\"from_modality\":" + (delta.from_present ? quote(modality_name(delta.from_modality)) : std::string("null")) +
             ",\"to_present\":" + (delta.to_present ? "true" : "false") +
             ",\"to_value\":" + (delta.to_present ? number(delta.to_value) : std::string("null")) +
             ",\"to_modality\":" + (delta.to_present ? quote(modality_name(delta.to_modality)) : std::string("null")) +
             ",\"strictness_effect\":" + quote(delta.strictness_effect) + "}";
    }
    out += "]";
    return out;
  };
  const auto strings_json = [](const std::vector<std::string>& values) {
    std::string out = "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (index != 0) {
        out += ",";
      }
      out += quote(values[index]);
    }
    out += "]";
    return out;
  };
  std::string out = "{\"class\":" + quote(report.class_id.str());
  out += ",\"from\":" + revision_json(report.from);
  out += ",\"to\":" + revision_json(report.to);
  out += ",\"relation\":" + quote(to_string(report.relation));
  out += ",\"identical_content\":" + std::string(report.identical_content ? "true" : "false");
  out += ",\"declared\":" + deltas_json(report.declared);
  out += ",\"effective\":" + deltas_json(report.effective);
  out += ",\"reference_changes\":" + strings_json(report.reference_changes);
  out += ",\"composition_changes\":" + strings_json(report.composition_changes);
  out += ",\"metadata_changes\":" + strings_json(report.metadata_changes);
  out += "}";
  return out;
}

std::string render_json(const ClassExplanation& explanation) {
  std::string obligations = "[";
  for (std::size_t index = 0; index < explanation.obligations.size(); ++index) {
    const ObligationExplanation& obligation = explanation.obligations[index];
    if (index != 0) {
      obligations += ",";
    }
    obligations += "{\"key\":" + quote(key_name(obligation.key)) +
                   ",\"modality\":" + quote(modality_name(obligation.modality)) +
                   ",\"value\":" + number(obligation.value) +
                   ",\"value_text\":" + quote(value_text(obligation.key, obligation.modality, obligation.value)) +
                   ",\"inherited\":" + (obligation.inherited ? "true" : "false") + ",\"provenance\":[";
    for (std::size_t p = 0; p < obligation.provenance.size(); ++p) {
      const Provenance& provenance = obligation.provenance[p];
      if (p != 0) {
        obligations += ",";
      }
      obligations += "{\"class\":" + quote(provenance.class_id.str()) +
                     ",\"revision\":" + revision_json(provenance.revision) +
                     ",\"modality\":" + quote(modality_name(provenance.modality)) +
                     ",\"value\":" + number(provenance.value) +
                     ",\"direct\":" + (provenance.direct ? "true" : "false") + "}";
    }
    obligations += "],\"suppressed\":[";
    for (std::size_t s = 0; s < obligation.suppressed.size(); ++s) {
      const Suppression& suppression = obligation.suppressed[s];
      if (s != 0) {
        obligations += ",";
      }
      obligations += "{\"class\":" + quote(suppression.class_id.str()) +
                     ",\"revision\":" + revision_json(suppression.revision) +
                     ",\"modality\":" + quote(modality_name(suppression.modality)) +
                     ",\"value\":" + number(suppression.value) +
                     ",\"reason\":" + quote(suppression.reason) + "}";
    }
    obligations += "]}";
  }
  obligations += "]";

  std::string rules = "[";
  for (std::size_t index = 0; index < explanation.rules.size(); ++index) {
    const RuleViolation& violation = explanation.rules[index];
    if (index != 0) {
      rules += ",";
    }
    rules += "{\"rule\":" + quote(violation.rule) + ",\"severity\":" + quote(to_string(violation.severity)) +
             ",\"code\":" + quote(to_string(violation.code)) + ",\"detail\":" + quote(violation.detail) + "}";
  }
  rules += "]";

  std::string resolution = "[";
  for (std::size_t index = 0; index < explanation.resolution_order.size(); ++index) {
    if (index != 0) {
      resolution += ",";
    }
    resolution += revision_json(explanation.resolution_order[index]);
  }
  resolution += "]";

  std::string references = "[";
  for (std::size_t index = 0; index < explanation.references.size(); ++index) {
    const ExternalReference& reference = explanation.references[index];
    if (index != 0) {
      references += ",";
    }
    references += "{\"kind\":" + quote(to_string(reference.kind)) + ",\"target\":" + quote(reference.target) +
                  ",\"bound\":" + (reference.is_bound() ? "true" : "false") +
                  ",\"digest\":" + (reference.is_bound() ? quote(reference.digest.hex()) : std::string("null")) +
                  ",\"authority\":" + (reference.authority.empty() ? std::string("null")
                                                                   : quote(reference.authority.str())) +
                  ",\"note\":" + (reference.note.empty() ? std::string("null") : quote(reference.note.value())) + "}";
  }
  references += "]";

  std::string composes = "[";
  for (std::size_t index = 0; index < explanation.composes.size(); ++index) {
    const CompositionRef& composition = explanation.composes[index];
    if (index != 0) {
      composes += ",";
    }
    composes += "{\"parent\":" + quote(composition.parent.str()) +
                ",\"revision\":" + revision_json(composition.revision) + "}";
  }
  composes += "]";

  std::string out = "{\"class\":" + quote(explanation.class_id.str());
  out += ",\"state\":" + quote(to_string(explanation.state));
  out += ",\"tip_present\":" + std::string(explanation.tip_present ? "true" : "false");
  if (explanation.tip_present) {
    out += ",\"authoritative\":" + revision_json(explanation.authoritative);
  }
  out += ",\"published_at\":" + timestamp_json(explanation.published_at);
  out += ",\"retired_at\":" + timestamp_json(explanation.retired_at);
  out += ",\"retire_reason\":" + quote(explanation.retire_reason);
  out += ",\"obligations\":" + obligations;
  out += ",\"references\":" + references;
  out += ",\"composition\":" + composes;
  out += ",\"rules\":" + rules;
  out += ",\"advisories\":" + diagnostics_json(explanation.advisories);
  out += ",\"resolution_order\":" + resolution;
  out += ",\"history\":" + history_json(explanation.history);
  out += "}";
  return out;
}

std::string render_json(const StoreInfo& info) {
  std::string out = "{\"root\":" + quote(info.root.string());
  out += ",\"mode\":" + quote(to_string(info.mode));
  out += ",\"authority\":" + quote(info.authority.str());
  out += ",\"epoch\":" + number(info.epoch.value());
  out += ",\"sequence\":" + number(info.sequence.value());
  out += ",\"manifest_digest\":" + quote(info.manifest_digest.hex());
  out += ",\"index_digest\":" + quote(info.index_digest.hex());
  out += ",\"written_at\":" + timestamp_json(info.written_at);
  out += ",\"classes\":" + std::to_string(info.class_count);
  out += ",\"revisions\":" + std::to_string(info.revision_count);
  out += ",\"record_files\":" + std::to_string(info.record_files);
  out += ",\"orphan_records\":" + std::to_string(info.orphan_records);
  out += ",\"staging_files_removed\":" + std::to_string(info.staging_files_removed);
  out += ",\"guard_repaired\":" + std::string(info.guard_repaired ? "true" : "false");
  out += ",\"lock_held_exclusive\":" + std::string(info.lock_held_exclusive ? "true" : "false");
  out += "}";
  return out;
}

std::string render_json(const CommitOutcome& outcome) {
  return "{\"sequence\":" + number(outcome.sequence.value()) + ",\"manifest_digest\":" +
         quote(outcome.manifest_digest.hex()) + ",\"records_written\":" + std::to_string(outcome.records_written) +
         ",\"idempotent\":" + (outcome.idempotent ? "true" : "false") + "}";
}

std::string render_json(const StoreVerifyReport& report) {
  std::string problems = "[";
  for (std::size_t index = 0; index < report.problems.size(); ++index) {
    if (index != 0) {
      problems += ",";
    }
    problems += quote(report.problems[index]);
  }
  problems += "]";
  std::string out = "{\"ok\":" + std::string(report.ok ? "true" : "false");
  out += ",\"sequence\":" + number(report.sequence.value());
  out += ",\"manifest_digest\":" + quote(report.manifest_digest.hex());
  out += ",\"referenced_records\":" + std::to_string(report.referenced_records);
  out += ",\"verified_records\":" + std::to_string(report.verified_records);
  out += ",\"orphan_records\":" + std::to_string(report.orphan_records);
  out += ",\"staging_files\":" + std::to_string(report.staging_files);
  out += ",\"problems\":" + problems;
  out += "}";
  return out;
}

std::string render_json(const StorePublishOutcome& outcome) {
  return "{\"publish\":" + render_json(outcome.publish) + ",\"commit\":" + render_json(outcome.commit) + "}";
}

std::string render_json(const StoreRetireOutcome& outcome) {
  return "{\"retire\":" + render_json(outcome.retire) + ",\"commit\":" + render_json(outcome.commit) + "}";
}

}  // namespace scr
