#pragma once

#include <string>

#include "scr/registry.hpp"
#include "scr/store.hpp"

namespace scr {

// Deterministic renderers. The text form is for operators; the JSON form is a
// stable machine-readable rendering for downstream consumers. Neither form is
// authority: the binding of record is the digest of the canonical frame.

std::string render_text(const PublishOutcome& outcome);
std::string render_text(const RetireOutcome& outcome);
std::string render_text(const BindingResolution& resolution);
std::string render_text(const ClassRecord& record);
std::string render_text(const CompareReport& report);
std::string render_text(const ClassExplanation& explanation);
std::string render_text(const StoreInfo& info);
std::string render_text(const CommitOutcome& outcome);
std::string render_text(const StoreVerifyReport& report);
std::string render_text(const StorePublishOutcome& outcome);
std::string render_text(const StoreRetireOutcome& outcome);

std::string render_json(const PublishOutcome& outcome);
std::string render_json(const RetireOutcome& outcome);
std::string render_json(const BindingResolution& resolution);
std::string render_json(const ClassRecord& record);
std::string render_json(const CompareReport& report);
std::string render_json(const ClassExplanation& explanation);
std::string render_json(const StoreInfo& info);
std::string render_json(const CommitOutcome& outcome);
std::string render_json(const StoreVerifyReport& report);
std::string render_json(const StorePublishOutcome& outcome);
std::string render_json(const StoreRetireOutcome& outcome);

// JSON string escaping (RFC 8259): quotes, backslashes, and control characters.
std::string json_escape(const std::string& text);

}  // namespace scr
