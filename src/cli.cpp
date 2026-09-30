// scr: the Service Class Registry command line tool.
//
// The CLI is a thin, strict front end over the library: every command maps onto
// a documented public API call, and the exit code is derived from the
// deterministic error taxonomy (see README.md, "Exit codes").

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "scr/platform.hpp"
#include "scr/registry.hpp"
#include "scr/report.hpp"
#include "scr/store.hpp"
#include "scr/text.hpp"
#include "scr/version.hpp"

// Implemented in bench.cpp.
int run_bench(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

namespace {

using scr::ClassId;
using scr::Digest;
using scr::ErrorCode;
using scr::Generation;
using scr::Registry;
using scr::Result;
using scr::Revision;
using scr::Status;
using scr::Store;
using scr::StoreMode;
using scr::Timestamp;

struct Options {
  std::map<std::string, std::string> values;
  std::set<std::string> flags;
  std::vector<std::string> positional;

  bool has(const std::string& name) const { return flags.find(name) != flags.end() || values.find(name) != values.end(); }

  std::string value_or(const std::string& name, const std::string& fallback) const {
    const auto found = values.find(name);
    return found == values.end() ? fallback : found->second;
  }

  Result<std::string> required(const std::string& name) const {
    const auto found = values.find(name);
    if (found == values.end()) {
      return Result<std::string>::failure(Status::failure(ErrorCode::InvalidArgument,
                                                          "option --" + name + " is required", "usage"));
    }
    return Result<std::string>::success(found->second);
  }
};

const char* const kUsage = R"(usage: scr <command> [options]

  scr version
  scr rules
  scr keys
  scr validate      --file <scx> [--json]
  scr store init    --store <dir> --authority <id> [--epoch <n>] [--at <unix-seconds>]
  scr store info    --store <dir> [--json]
  scr store verify  --store <dir> [--json]
  scr store compact --store <dir> [--json]
  scr store adopt-epoch --store <dir> --epoch <n> [--at <unix-seconds>] [--json]
  scr store lock-probe --store <dir> --hold-ms <n>
  scr class publish --store <dir> --file <scx> [--allow-new-generation] [--allow-retired-parents]
                    [--at <unix-seconds>] [--json]
  scr class show    --store <dir> --class <id> [--json]
  scr class explain --store <dir> --class <id> [--json]
  scr class history --store <dir> --class <id> [--json]
  scr class compare --store <dir> --class <id> --from <generation.revision[@digest]>
                    --to <generation.revision[@digest]> [--json]
  scr class retire  --store <dir> --class <id> --reason <text> [--at <unix-seconds>] [--json]
  scr class export  --store <dir> --class <id> [--generation <n>] [--revision <n>]
                    [--format scx|hex] [--out <file>]
  scr bind          --store <dir> --class <id> [--generation <n>] [--revision <n>] [--digest <hex>] [--json]
  scr check         --store <dir> --class <id> --generation <n> --revision <n> --digest <hex> [--json]
  scr bench         [--iterations <n>] [--json]
  scr help [command]
)";

int usage_error(const std::string& message) {
  std::cerr << "scr: error: " << message << "\n\n" << kUsage;
  return 2;
}

int exit_code_for(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok:
      return 0;
    case ErrorCode::UnknownClass:
    case ErrorCode::UnknownRevision:
      return 4;
    case ErrorCode::StaleTip:
    case ErrorCode::RetiredClass:
    case ErrorCode::DigestMismatch:
      return 5;
    case ErrorCode::StoreBusy:
    case ErrorCode::CorruptStore:
    case ErrorCode::RollbackDetected:
    case ErrorCode::StoreNotInitialized:
    case ErrorCode::StaleAuthority:
    case ErrorCode::AlreadyInitialized:
      return 6;
    case ErrorCode::IoFailure:
    case ErrorCode::Internal:
      return 7;
    default:
      return 3;
  }
}

void report_status(const Status& status) {
  std::cerr << "scr: error: " << scr::to_string(status.code());
  if (!status.subject().empty()) {
    std::cerr << " [" << status.subject() << "]";
  }
  if (!status.detail().empty()) {
    std::cerr << ": " << status.detail();
  }
  std::cerr << "\n";
  for (std::size_t index = 1; index < status.diagnostics().size(); ++index) {
    const scr::Diagnostic& diagnostic = status.diagnostics()[index];
    std::cerr << "scr: note: " << scr::to_string(diagnostic.severity) << " " << scr::to_string(diagnostic.code);
    if (!diagnostic.subject.empty()) {
      std::cerr << " [" << diagnostic.subject << "]";
    }
    if (!diagnostic.detail.empty()) {
      std::cerr << ": " << diagnostic.detail;
    }
    std::cerr << "\n";
  }
}

Timestamp now_timestamp() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
  if (seconds < 0) {
    return Timestamp::unset();
  }
  const auto stamp = Timestamp::from_unix_seconds(seconds);
  return stamp.ok() ? stamp.value() : Timestamp::unset();
}

Result<Timestamp> timestamp_from_option(const Options& options, const std::string& name, bool* provided) {
  const auto found = options.values.find(name);
  if (found == options.values.end()) {
    if (provided != nullptr) {
      *provided = false;
    }
    return Result<Timestamp>::success(now_timestamp());
  }
  if (provided != nullptr) {
    *provided = true;
  }
  if (found->second.empty()) {
    return Result<Timestamp>::failure(
        Status::failure(ErrorCode::InvalidArgument, "--" + name + " requires a value", "usage"));
  }
  std::int64_t seconds = 0;
  std::size_t index = 0;
  bool negative = false;
  if (found->second[0] == '-') {
    negative = true;
    index = 1;
  }
  if (index >= found->second.size()) {
    return Result<Timestamp>::failure(
        Status::failure(ErrorCode::InvalidArgument, "--" + name + " must be an integer", "usage"));
  }
  for (; index < found->second.size(); ++index) {
    const char character = found->second[index];
    if (character < '0' || character > '9') {
      return Result<Timestamp>::failure(
          Status::failure(ErrorCode::InvalidArgument, "--" + name + " must be an integer", "usage"));
    }
    seconds = seconds * 10 + (character - '0');
    if (seconds > 253402300799LL) {
      return Result<Timestamp>::failure(
          Status::failure(ErrorCode::OutOfRange, "--" + name + " is out of range", "usage"));
    }
  }
  if (negative) {
    seconds = -seconds;
  }
  const auto stamp = Timestamp::from_unix_seconds(seconds);
  if (!stamp.ok()) {
    return Result<Timestamp>::failure(stamp.status());
  }
  return Result<Timestamp>::success(stamp.value());
}

Result<std::uint64_t> unsigned_from_option(const Options& options, const std::string& name) {
  const auto found = options.values.find(name);
  if (found == options.values.end() || found->second.empty()) {
    return Result<std::uint64_t>::failure(
        Status::failure(ErrorCode::InvalidArgument, "option --" + name + " requires a non-negative integer", "usage"));
  }
  std::uint64_t value = 0;
  for (const char character : found->second) {
    if (character < '0' || character > '9') {
      return Result<std::uint64_t>::failure(
          Status::failure(ErrorCode::InvalidArgument, "option --" + name + " must be a non-negative integer", "usage"));
    }
    const auto scaled = scr::checked_mul(value, 10);
    if (!scaled.ok()) {
      return Result<std::uint64_t>::failure(scaled.status());
    }
    const auto added = scr::checked_add(scaled.value(), static_cast<std::uint64_t>(character - '0'));
    if (!added.ok()) {
      return Result<std::uint64_t>::failure(added.status());
    }
    value = added.value();
  }
  return Result<std::uint64_t>::success(value);
}

Result<std::filesystem::path> store_path(const Options& options) {
  const auto value = options.required("store");
  if (!value.ok()) {
    return Result<std::filesystem::path>::failure(value.status());
  }
  return scr::platform::path_from_utf8(value.value());
}

Result<ClassId> class_option(const Options& options) {
  const auto value = options.required("class");
  if (!value.ok()) {
    return Result<ClassId>::failure(value.status());
  }
  return ClassId::parse(value.value());
}

// "generation.revision" or "generation.revision@digest"
Result<scr::RevisionRef> revision_option(const Options& options, const std::string& name, const Digest& fallback_digest) {
  const auto text = options.required(name);
  if (!text.ok()) {
    return Result<scr::RevisionRef>::failure(text.status());
  }
  const std::string& raw = text.value();
  const std::size_t at = raw.find('@');
  const std::string head = (at == std::string::npos) ? raw : raw.substr(0, at);
  const std::size_t dot = head.find('.');
  if (dot == std::string::npos) {
    return Result<scr::RevisionRef>::failure(Status::failure(
        ErrorCode::InvalidArgument, "--" + name + " must be <generation>.<revision>[@<digest>]", "usage"));
  }
  const auto parse_part = [&](const std::string& part) -> Result<std::uint64_t> {
    if (part.empty()) {
      return Result<std::uint64_t>::failure(
          Status::failure(ErrorCode::InvalidArgument, "--" + name + " has an empty component", "usage"));
    }
    std::uint64_t value = 0;
    for (const char character : part) {
      if (character < '0' || character > '9') {
        return Result<std::uint64_t>::failure(Status::failure(
            ErrorCode::InvalidArgument, "--" + name + " must contain decimal integers", "usage"));
      }
      const auto scaled = scr::checked_mul(value, 10);
      if (!scaled.ok()) {
        return Result<std::uint64_t>::failure(scaled.status());
      }
      const auto added = scr::checked_add(scaled.value(), static_cast<std::uint64_t>(character - '0'));
      if (!added.ok()) {
        return Result<std::uint64_t>::failure(added.status());
      }
      value = added.value();
    }
    return Result<std::uint64_t>::success(value);
  };
  const auto generation = parse_part(head.substr(0, dot));
  if (!generation.ok()) {
    return Result<scr::RevisionRef>::failure(generation.status());
  }
  const auto revision = parse_part(head.substr(dot + 1));
  if (!revision.ok()) {
    return Result<scr::RevisionRef>::failure(revision.status());
  }
  if (generation.value() == 0 || revision.value() == 0 || revision.value() > 0xFFFFFFFFull) {
    return Result<scr::RevisionRef>::failure(
        Status::failure(ErrorCode::OutOfRange, "--" + name + " components must be non-zero and in range", "usage"));
  }
  Digest digest = fallback_digest;
  if (at != std::string::npos) {
    const auto parsed = Digest::parse(raw.substr(at + 1));
    if (!parsed.ok()) {
      return Result<scr::RevisionRef>::failure(
          Status::failure(ErrorCode::InvalidArgument, "--" + name + " digest must be 64 hexadecimal characters",
                          "usage"));
    }
    digest = parsed.value();
  }
  scr::RevisionRef reference;
  reference.generation = Generation::from(generation.value());
  reference.revision = Revision::from(static_cast<std::uint32_t>(revision.value()));
  reference.digest = digest;
  return Result<scr::RevisionRef>::success(reference);
}

Status parse_arguments(const std::vector<std::string>& args, const std::set<std::string>& value_options,
                       const std::set<std::string>& flag_options, Options* options) {
  options->values.clear();
  options->flags.clear();
  options->positional.clear();
  for (std::size_t index = 0; index < args.size(); ++index) {
    const std::string& token = args[index];
    if (token.size() < 3 || token[0] != '-' || token[1] != '-') {
      return Status::failure(ErrorCode::InvalidArgument,
                             "unexpected argument '" + token + "' (options are introduced by --)", "usage");
    }
    const std::string body = token.substr(2);
    const std::size_t equals = body.find('=');
    const std::string name = (equals == std::string::npos) ? body : body.substr(0, equals);
    if (value_options.find(name) != value_options.end()) {
      std::string value;
      if (equals != std::string::npos) {
        value = body.substr(equals + 1);
      } else {
        if (index + 1 >= args.size()) {
          return Status::failure(ErrorCode::InvalidArgument, "--" + name + " requires a value", "usage");
        }
        value = args[++index];
      }
      options->values[name] = value;
      continue;
    }
    if (flag_options.find(name) != flag_options.end()) {
      if (equals != std::string::npos) {
        return Status::failure(ErrorCode::InvalidArgument, "--" + name + " does not take a value", "usage");
      }
      options->flags.insert(name);
      continue;
    }
    return Status::failure(ErrorCode::InvalidArgument, "unknown option --" + name, "usage");
  }
  return Status::success();
}

Result<Store> open_store(const Options& options, StoreMode mode) {
  const auto path = store_path(options);
  if (!path.ok()) {
    return Result<Store>::failure(path.status());
  }
  return Store::open(path.value(), mode);
}

int command_version() {
  std::cout << scr::kProductName << " " << scr::kVersionString << "\n";
  std::cout << "canonical frame format: " << scr::kFrameFormatVersion << "\n";
  std::cout << "store format: " << scr::kStoreFormatVersion << "\n";
  std::cout << "canonical text format: " << static_cast<unsigned>(scr::kCanonicalTextVersion) << "\n";
  std::cout << scr::kCopyrightLine << "\n";
  std::cout << "Apache License 2.0. No telemetry transmission.\n";
  return 0;
}

int command_rules() {
  std::cout << "cross-key consistency rules (" << scr::rule_count() << "):\n";
  for (std::size_t index = 0; index < scr::rule_count(); ++index) {
    const char* id = scr::rule_id_at(index);
    std::cout << "  " << id << "  " << scr::rule_rationale(id) << "\n";
  }
  return 0;
}

int command_keys() {
  for (std::size_t index = 0; index < scr::kObligationKeyCount; ++index) {
    const scr::KeyDescriptor& descriptor = scr::key_at(index);
    std::cout << descriptor.name << "  " << static_cast<unsigned>(descriptor.key) << "  ";
    switch (descriptor.kind) {
      case scr::ValueKind::UnsignedInteger:
        std::cout << "integer[" << descriptor.min_value << "," << descriptor.max_value << "]";
        break;
      case scr::ValueKind::Enumerated:
        std::cout << "enum{";
        for (std::size_t choice = 0; choice < descriptor.enum_count; ++choice) {
          if (choice != 0) {
            std::cout << "|";
          }
          std::cout << descriptor.enum_names[choice].name;
        }
        std::cout << "}";
        break;
    }
    switch (descriptor.strictness) {
      case scr::Strictness::HigherIsStricter:
        std::cout << " higher-is-stricter";
        break;
      case scr::Strictness::LowerIsStricter:
        std::cout << " lower-is-stricter";
        break;
      case scr::Strictness::Incomparable:
        std::cout << " incomparable";
        break;
    }
    std::cout << "\n";
  }
  return 0;
}

int command_validate(const Options& options) {
  const auto file = options.required("file");
  if (!file.ok()) {
    return usage_error(file.status().detail());
  }
  const auto path = scr::platform::path_from_utf8(file.value());
  if (!path.ok()) {
    report_status(path.status());
    return exit_code_for(path.status().code());
  }
  const auto bytes = scr::platform::read_file(path.value(), scr::kMaxScxBytes);
  if (!bytes.ok()) {
    report_status(bytes.status());
    return exit_code_for(bytes.status().code());
  }
  const std::string text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
  const auto revision = scr::parse_scx(text);
  if (!revision.ok()) {
    report_status(revision.status());
    return exit_code_for(revision.status().code());
  }
  const auto digest = scr::revision_frame_digest(revision.value());
  if (!digest.ok()) {
    report_status(digest.status());
    return exit_code_for(digest.status().code());
  }
  if (options.has("json")) {
    std::cout << "{\"valid\":true,\"class\":\"" << scr::json_escape(revision.value().class_id.str())
              << "\",\"digest\":\"" << digest.value().hex() << "\"}\n";
    return 0;
  }
  std::cout << "valid: " << revision.value().class_id.str() << " "
            << revision.value().generation.value() << "." << revision.value().revision.value() << "\n";
  std::cout << "digest: " << digest.value().hex() << "\n";
  std::cout << "obligations: " << revision.value().obligations.size() << "\n";
  std::cout << "references: " << revision.value().references.size() << "\n";
  std::cout << "composes: " << revision.value().composes.size() << "\n";
  return 0;
}

int command_store(const std::string& action, const Options& options) {
  if (action == "init") {
    const auto path = store_path(options);
    if (!path.ok()) {
      return usage_error(path.status().detail());
    }
    const auto authority_text = options.required("authority");
    if (!authority_text.ok()) {
      return usage_error(authority_text.status().detail());
    }
    const auto authority = scr::AuthorityId::parse(authority_text.value());
    if (!authority.ok()) {
      report_status(authority.status());
      return exit_code_for(authority.status().code());
    }
    std::uint64_t epoch_value = 1;
    if (options.has("epoch")) {
      const auto parsed = unsigned_from_option(options, "epoch");
      if (!parsed.ok()) {
        return usage_error(parsed.status().detail());
      }
      epoch_value = parsed.value();
    }
    const auto at = timestamp_from_option(options, "at", nullptr);
    if (!at.ok()) {
      return usage_error(at.status().detail());
    }
    auto store = Store::create(path.value(), authority.value(), scr::Epoch::from(epoch_value), at.value());
    if (!store.ok()) {
      report_status(store.status());
      return exit_code_for(store.status().code());
    }
    if (options.has("json")) {
      std::cout << scr::render_json(store.value().info()) << "\n";
    } else {
      std::cout << scr::render_text(store.value().info());
    }
    return 0;
  }

  if (action == "info") {
    auto store = open_store(options, StoreMode::ReadOnly);
    if (!store.ok()) {
      report_status(store.status());
      return exit_code_for(store.status().code());
    }
    if (options.has("json")) {
      std::cout << scr::render_json(store.value().info()) << "\n";
    } else {
      std::cout << scr::render_text(store.value().info());
    }
    return 0;
  }

  if (action == "verify") {
    auto store = open_store(options, StoreMode::ReadOnly);
    if (!store.ok()) {
      report_status(store.status());
      return exit_code_for(store.status().code());
    }
    const auto report = store.value().verify();
    if (!report.ok()) {
      report_status(report.status());
      return exit_code_for(report.status().code());
    }
    if (options.has("json")) {
      std::cout << scr::render_json(report.value()) << "\n";
    } else {
      std::cout << scr::render_text(report.value());
    }
    return report.value().ok ? 0 : 6;
  }

  if (action == "compact") {
    auto store = open_store(options, StoreMode::ReadWrite);
    if (!store.ok()) {
      report_status(store.status());
      return exit_code_for(store.status().code());
    }
    const auto removed = store.value().compact();
    if (!removed.ok()) {
      report_status(removed.status());
      return exit_code_for(removed.status().code());
    }
    if (options.has("json")) {
      std::cout << "{\"removed\":" << removed.value() << "}\n";
    } else {
      std::cout << "removed " << removed.value() << " unreferenced files\n";
    }
    return 0;
  }

  if (action == "adopt-epoch") {
    auto store = open_store(options, StoreMode::ReadWrite);
    if (!store.ok()) {
      report_status(store.status());
      return exit_code_for(store.status().code());
    }
    const auto epoch_value = unsigned_from_option(options, "epoch");
    if (!epoch_value.ok()) {
      return usage_error(epoch_value.status().detail());
    }
    const auto at = timestamp_from_option(options, "at", nullptr);
    if (!at.ok()) {
      return usage_error(at.status().detail());
    }
    const auto committed = store.value().adopt_epoch(scr::Epoch::from(epoch_value.value()), at.value());
    if (!committed.ok()) {
      report_status(committed.status());
      return exit_code_for(committed.status().code());
    }
    if (options.has("json")) {
      std::cout << scr::render_json(committed.value()) << "\n";
    } else {
      std::cout << scr::render_text(committed.value());
    }
    return 0;
  }

  if (action == "lock-probe") {
    const auto path = store_path(options);
    if (!path.ok()) {
      return usage_error(path.status().detail());
    }
    const auto hold = unsigned_from_option(options, "hold-ms");
    if (!hold.ok()) {
      return usage_error(hold.status().detail());
    }
    auto lock = scr::platform::LockFile::acquire(path.value() / "LOCK", scr::platform::LockMode::Exclusive);
    if (!lock.ok()) {
      report_status(lock.status());
      return exit_code_for(lock.status().code());
    }
    std::cout << "lock acquired by pid " << scr::platform::process_id_text() << "; holding for " << hold.value()
              << " ms\n";
    std::cout.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(hold.value()));
    const auto released = lock.value().release();
    if (!released.ok()) {
      report_status(released.status());
      return exit_code_for(released.status().code());
    }
    std::cout << "lock released\n";
    return 0;
  }

  return usage_error("unknown store action '" + action + "'");
}

int command_class(const std::string& action, const Options& options) {
  if (action == "publish") {
    auto store = open_store(options, StoreMode::ReadWrite);
    if (!store.ok()) {
      report_status(store.status());
      return exit_code_for(store.status().code());
    }
    const auto file = options.required("file");
    if (!file.ok()) {
      return usage_error(file.status().detail());
    }
    const auto path = scr::platform::path_from_utf8(file.value());
    if (!path.ok()) {
      report_status(path.status());
      return exit_code_for(path.status().code());
    }
    const auto bytes = scr::platform::read_file(path.value(), scr::kMaxScxBytes);
    if (!bytes.ok()) {
      report_status(bytes.status());
      return exit_code_for(bytes.status().code());
    }
    const std::string text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
    const auto revision = scr::parse_scx(text);
    if (!revision.ok()) {
      report_status(revision.status());
      return exit_code_for(revision.status().code());
    }
    const auto at = timestamp_from_option(options, "at", nullptr);
    if (!at.ok()) {
      return usage_error(at.status().detail());
    }
    scr::PublishOptions publish_options;
    publish_options.allow_new_generation = options.has("allow-new-generation");
    publish_options.allow_retired_parents = options.has("allow-retired-parents");
    publish_options.at = at.value();

    const auto outcome = store.value().publish(revision.value(), publish_options);
    if (!outcome.ok()) {
      report_status(outcome.status());
      return exit_code_for(outcome.status().code());
    }
    if (options.has("json")) {
      std::cout << scr::render_json(outcome.value()) << "\n";
    } else {
      std::cout << scr::render_text(outcome.value());
    }
    return 0;
  }

  if (action == "show" || action == "history") {
    auto store = open_store(options, StoreMode::ReadOnly);
    if (!store.ok()) {
      report_status(store.status());
      return exit_code_for(store.status().code());
    }
    const auto class_id = class_option(options);
    if (!class_id.ok()) {
      return usage_error(class_id.status().detail());
    }
    const auto record = store.value().registry().class_record(class_id.value());
    if (!record.ok()) {
      report_status(record.status());
      return exit_code_for(record.status().code());
    }
    if (options.has("json")) {
      std::cout << scr::render_json(record.value()) << "\n";
    } else {
      std::cout << scr::render_text(record.value());
    }
    return 0;
  }

  if (action == "explain") {
    auto store = open_store(options, StoreMode::ReadOnly);
    if (!store.ok()) {
      report_status(store.status());
      return exit_code_for(store.status().code());
    }
    const auto class_id = class_option(options);
    if (!class_id.ok()) {
      return usage_error(class_id.status().detail());
    }
    const auto explanation = store.value().registry().explain(class_id.value());
    if (!explanation.ok()) {
      report_status(explanation.status());
      return exit_code_for(explanation.status().code());
    }
    if (options.has("json")) {
      std::cout << scr::render_json(explanation.value()) << "\n";
    } else {
      std::cout << scr::render_text(explanation.value());
    }
    return 0;
  }

  if (action == "compare") {
    auto store = open_store(options, StoreMode::ReadOnly);
    if (!store.ok()) {
      report_status(store.status());
      return exit_code_for(store.status().code());
    }
    const auto class_id = class_option(options);
    if (!class_id.ok()) {
      return usage_error(class_id.status().detail());
    }
    const auto from = revision_option(options, "from", Digest::null());
    if (!from.ok()) {
      return usage_error(from.status().detail());
    }
    const auto to = revision_option(options, "to", Digest::null());
    if (!to.ok()) {
      return usage_error(to.status().detail());
    }
    scr::RevisionRef from_reference = from.value();
    scr::RevisionRef to_reference = to.value();
    const auto lookup_from =
        store.value().registry().get(class_id.value(), from_reference.generation, from_reference.revision);
    const auto lookup_to =
        store.value().registry().get(class_id.value(), to_reference.generation, to_reference.revision);
    if (!lookup_from.ok()) {
      report_status(lookup_from.status());
      return exit_code_for(lookup_from.status().code());
    }
    if (!lookup_to.ok()) {
      report_status(lookup_to.status());
      return exit_code_for(lookup_to.status().code());
    }
    if (!from_reference.digest.is_null() && from_reference.digest != lookup_from.value().digest) {
      return usage_error("--from names digest " + from_reference.digest.hex() + " but the published revision is " +
                         lookup_from.value().digest.hex());
    }
    if (!to_reference.digest.is_null() && to_reference.digest != lookup_to.value().digest) {
      return usage_error("--to names digest " + to_reference.digest.hex() + " but the published revision is " +
                         lookup_to.value().digest.hex());
    }
    from_reference.digest = lookup_from.value().digest;
    to_reference.digest = lookup_to.value().digest;
    const auto report = store.value().registry().compare(from_reference, to_reference);
    if (!report.ok()) {
      report_status(report.status());
      return exit_code_for(report.status().code());
    }
    if (options.has("json")) {
      std::cout << scr::render_json(report.value()) << "\n";
    } else {
      std::cout << scr::render_text(report.value());
    }
    return 0;
  }

  if (action == "retire") {
    auto store = open_store(options, StoreMode::ReadWrite);
    if (!store.ok()) {
      report_status(store.status());
      return exit_code_for(store.status().code());
    }
    const auto class_id = class_option(options);
    if (!class_id.ok()) {
      return usage_error(class_id.status().detail());
    }
    const auto reason_text = options.required("reason");
    if (!reason_text.ok()) {
      return usage_error(reason_text.status().detail());
    }
    const auto reason = scr::BoundedText::create(reason_text.value(), scr::kRetireReasonMaxBytes);
    if (!reason.ok()) {
      report_status(reason.status());
      return exit_code_for(reason.status().code());
    }
    const auto at = timestamp_from_option(options, "at", nullptr);
    if (!at.ok()) {
      return usage_error(at.status().detail());
    }
    const auto outcome = store.value().retire(class_id.value(), reason.value(), at.value());
    if (!outcome.ok()) {
      report_status(outcome.status());
      return exit_code_for(outcome.status().code());
    }
    if (options.has("json")) {
      std::cout << scr::render_json(outcome.value()) << "\n";
    } else {
      std::cout << scr::render_text(outcome.value());
    }
    return 0;
  }

  if (action == "export") {
    auto store = open_store(options, StoreMode::ReadOnly);
    if (!store.ok()) {
      report_status(store.status());
      return exit_code_for(store.status().code());
    }
    const auto class_id = class_option(options);
    if (!class_id.ok()) {
      return usage_error(class_id.status().detail());
    }
    const auto record = store.value().registry().class_record(class_id.value());
    if (!record.ok()) {
      report_status(record.status());
      return exit_code_for(record.status().code());
    }
    scr::Generation generation = record.value().generation;
    if (options.has("generation")) {
      const auto parsed = unsigned_from_option(options, "generation");
      if (!parsed.ok()) {
        return usage_error(parsed.status().detail());
      }
      generation = Generation::from(parsed.value());
    }
    scr::Revision revision = record.value().tip;
    if (options.has("revision")) {
      const auto parsed = unsigned_from_option(options, "revision");
      if (!parsed.ok()) {
        return usage_error(parsed.status().detail());
      }
      if (parsed.value() > 0xFFFFFFFFull) {
        return usage_error("--revision is out of range");
      }
      revision = Revision::from(static_cast<std::uint32_t>(parsed.value()));
    }
    const auto content = store.value().registry().get(class_id.value(), generation, revision);
    if (!content.ok()) {
      report_status(content.status());
      return exit_code_for(content.status().code());
    }
    const std::string format = options.value_or("format", "scx");
    std::string rendered;
    if (format == "scx") {
      rendered = scr::emit_scx(content.value().content);
    } else if (format == "hex") {
      rendered = scr::emit_frame_hex(content.value().content);
      rendered += "\n";
    } else {
      return usage_error("--format must be scx or hex");
    }
    if (options.has("out")) {
      const auto out_path = scr::platform::path_from_utf8(options.value_or("out", ""));
      if (!out_path.ok()) {
        report_status(out_path.status());
        return exit_code_for(out_path.status().code());
      }
      std::vector<std::uint8_t> bytes(rendered.begin(), rendered.end());
      const auto staging = out_path.value().string() + ".tmp";
      const auto staging_path = scr::platform::path_from_utf8(staging);
      if (!staging_path.ok()) {
        report_status(staging_path.status());
        return exit_code_for(staging_path.status().code());
      }
      const auto written = scr::platform::write_file_staged(staging_path.value(), bytes);
      if (!written.ok()) {
        report_status(written.status());
        return exit_code_for(written.status().code());
      }
      const auto published = scr::platform::publish_staged(staging_path.value(), out_path.value(), true);
      if (!published.ok()) {
        report_status(published.status());
        return exit_code_for(published.status().code());
      }
      std::cout << "wrote " << rendered.size() << " bytes to " << scr::platform::path_to_utf8(out_path.value()) << "\n";
      return 0;
    }
    std::cout << rendered;
    return 0;
  }

  return usage_error("unknown class action '" + action + "'");
}

int command_bind(const Options& options) {
  auto store = open_store(options, StoreMode::ReadOnly);
  if (!store.ok()) {
    report_status(store.status());
    return exit_code_for(store.status().code());
  }
  const auto class_id = class_option(options);
  if (!class_id.ok()) {
    return usage_error(class_id.status().detail());
  }
  const auto record = store.value().registry().class_record(class_id.value());
  if (!record.ok()) {
    report_status(record.status());
    return exit_code_for(record.status().code());
  }
  scr::Generation generation = record.value().generation;
  if (options.has("generation")) {
    const auto parsed = unsigned_from_option(options, "generation");
    if (!parsed.ok()) {
      return usage_error(parsed.status().detail());
    }
    generation = Generation::from(parsed.value());
  }
  scr::Revision revision = record.value().tip;
  if (options.has("revision")) {
    const auto parsed = unsigned_from_option(options, "revision");
    if (!parsed.ok()) {
      return usage_error(parsed.status().detail());
    }
    if (parsed.value() > 0xFFFFFFFFull) {
      return usage_error("--revision is out of range");
    }
    revision = Revision::from(static_cast<std::uint32_t>(parsed.value()));
  }
  Digest digest = record.value().tip_digest;
  if (options.has("digest")) {
    const auto parsed = Digest::parse(options.value_or("digest", ""));
    if (!parsed.ok()) {
      return usage_error("--digest must be 64 hexadecimal characters");
    }
    digest = parsed.value();
  } else if (generation != record.value().generation || revision != record.value().tip) {
    return usage_error("binding a historical revision requires --digest");
  }
  scr::ClassBinding binding;
  binding.class_id = class_id.value();
  binding.revision.generation = generation;
  binding.revision.revision = revision;
  binding.revision.digest = digest;
  const auto resolution = store.value().registry().resolve(binding);
  if (!resolution.ok()) {
    report_status(resolution.status());
    return exit_code_for(resolution.status().code());
  }
  if (options.has("json")) {
    std::cout << scr::render_json(resolution.value()) << "\n";
  } else {
    std::cout << "binding " << binding.class_id.str() << " " << binding.revision.to_string() << "\n";
    std::cout << scr::render_text(resolution.value());
  }
  return 0;
}

int command_check(const Options& options) {
  auto store = open_store(options, StoreMode::ReadOnly);
  if (!store.ok()) {
    report_status(store.status());
    return exit_code_for(store.status().code());
  }
  const auto class_id = class_option(options);
  if (!class_id.ok()) {
    return usage_error(class_id.status().detail());
  }
  const auto generation_value = unsigned_from_option(options, "generation");
  if (!generation_value.ok()) {
    return usage_error(generation_value.status().detail());
  }
  const auto revision_value = unsigned_from_option(options, "revision");
  if (!revision_value.ok()) {
    return usage_error(revision_value.status().detail());
  }
  if (revision_value.value() == 0 || revision_value.value() > 0xFFFFFFFFull) {
    return usage_error("--revision is out of range");
  }
  const auto digest_text = options.required("digest");
  if (!digest_text.ok()) {
    return usage_error(digest_text.status().detail());
  }
  const auto digest = Digest::parse(digest_text.value());
  if (!digest.ok()) {
    return usage_error("--digest must be 64 hexadecimal characters");
  }
  scr::ClassBinding binding;
  binding.class_id = class_id.value();
  binding.revision.generation = Generation::from(generation_value.value());
  binding.revision.revision = Revision::from(static_cast<std::uint32_t>(revision_value.value()));
  binding.revision.digest = digest.value();
  const auto resolution = store.value().registry().resolve(binding);
  if (!resolution.ok()) {
    report_status(resolution.status());
    return exit_code_for(resolution.status().code());
  }
  if (options.has("json")) {
    std::cout << scr::render_json(resolution.value()) << "\n";
  } else {
    std::cout << scr::render_text(resolution.value());
  }
  switch (resolution.value().state) {
    case scr::BindingState::Fresh:
      return 0;
    case scr::BindingState::UnknownClass:
    case scr::BindingState::UnknownRevision:
      return 4;
    default:
      return 5;
  }
}

}  // namespace

int run_cli(const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cout << kUsage;
    return 0;
  }
  const std::string& command = args[0];
  const std::vector<std::string> rest(args.begin() + 1, args.end());

  Options options;
  const std::set<std::string> no_flags;

  if (command == "help" || command == "--help" || command == "-h") {
    std::cout << kUsage;
    return 0;
  }
  if (command == "version" || command == "--version") {
    return command_version();
  }
  if (command == "rules") {
    return command_rules();
  }
  if (command == "keys") {
    return command_keys();
  }
  if (command == "validate") {
    const Status parsed = parse_arguments(rest, {"file"}, {"json"}, &options);
    if (!parsed.ok()) {
      return usage_error(parsed.detail());
    }
    return command_validate(options);
  }
  if (command == "store") {
    if (rest.empty()) {
      return usage_error("store requires an action");
    }
    const std::string action = rest[0];
    const std::vector<std::string> action_args(rest.begin() + 1, rest.end());
    Status parsed;
    if (action == "init") {
      parsed = parse_arguments(action_args, {"store", "authority", "epoch", "at"}, {"json"}, &options);
    } else if (action == "info" || action == "verify" || action == "compact") {
      parsed = parse_arguments(action_args, {"store"}, {"json"}, &options);
    } else if (action == "adopt-epoch") {
      parsed = parse_arguments(action_args, {"store", "epoch", "at"}, {"json"}, &options);
    } else if (action == "lock-probe") {
      parsed = parse_arguments(action_args, {"store", "hold-ms"}, no_flags, &options);
    } else {
      return usage_error("unknown store action '" + action + "'");
    }
    if (!parsed.ok()) {
      return usage_error(parsed.detail());
    }
    return command_store(action, options);
  }
  if (command == "class") {
    if (rest.empty()) {
      return usage_error("class requires an action");
    }
    const std::string action = rest[0];
    const std::vector<std::string> action_args(rest.begin() + 1, rest.end());
    Status parsed;
    if (action == "publish") {
      parsed = parse_arguments(action_args, {"store", "file", "at"},
                               {"json", "allow-new-generation", "allow-retired-parents"}, &options);
    } else if (action == "show" || action == "explain" || action == "history") {
      parsed = parse_arguments(action_args, {"store", "class"}, {"json"}, &options);
    } else if (action == "compare") {
      parsed = parse_arguments(action_args, {"store", "class", "from", "to"}, {"json"}, &options);
    } else if (action == "retire") {
      parsed = parse_arguments(action_args, {"store", "class", "reason", "at"}, {"json"}, &options);
    } else if (action == "export") {
      parsed = parse_arguments(action_args, {"store", "class", "generation", "revision", "format", "out"}, no_flags,
                               &options);
    } else {
      return usage_error("unknown class action '" + action + "'");
    }
    if (!parsed.ok()) {
      return usage_error(parsed.detail());
    }
    return command_class(action, options);
  }
  if (command == "bind") {
    const Status parsed = parse_arguments(rest, {"store", "class", "generation", "revision", "digest"}, {"json"},
                                          &options);
    if (!parsed.ok()) {
      return usage_error(parsed.detail());
    }
    return command_bind(options);
  }
  if (command == "check") {
    const Status parsed =
        parse_arguments(rest, {"store", "class", "generation", "revision", "digest"}, {"json"}, &options);
    if (!parsed.ok()) {
      return usage_error(parsed.detail());
    }
    return command_check(options);
  }
  if (command == "bench") {
    return run_bench(rest, std::cout, std::cerr);
  }
  return usage_error("unknown command '" + command + "'");
}
