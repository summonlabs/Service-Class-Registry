#include "harness.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "scr/text.hpp"

namespace scrtest {
namespace {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

std::uint64_t g_seed = 20260101;

int process_id() {
#ifdef _WIN32
  return _getpid();
#else
  return static_cast<int>(getpid());
#endif
}

std::filesystem::path unique_temp_path(const std::string& label) {
  static std::uint64_t counter = 0;
  ++counter;
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  const std::string name = "scr-test-" + std::to_string(process_id()) + "-" + label + "-" + std::to_string(counter);
  return base / name;
}

}  // namespace

void register_test(const std::string& name, TestFunction function) {
  registry().push_back(TestCase{name, function});
}

std::uint64_t current_seed() {
  return g_seed;
}

void Context::note_failure(const std::string& message, const char* file, int line) {
  std::ostringstream text;
  text << file << ":" << line << ": " << message;
  failures.push_back(text.str());
}

bool Context::check(bool condition, const char* expression, const char* file, int line) {
  ++checks;
  if (condition) {
    return true;
  }
  note_failure(std::string("check failed: ") + expression, file, line);
  return false;
}

void Context::fail(const std::string& message, const char* file, int line) {
  note_failure(message, file, line);
}

std::string render_value(bool value) {
  return value ? "true" : "false";
}

std::string render_value(const char* value) {
  return value == nullptr ? "<null>" : std::string(value);
}

std::string render_value(const std::string& value) {
  return "\"" + value + "\"";
}

std::string render_value(std::string_view value) {
  return "\"" + std::string(value) + "\"";
}

std::string render_value(int value) {
  return std::to_string(value);
}

std::string render_value(unsigned int value) {
  return std::to_string(value);
}

std::string render_value(std::uint64_t value) {
  return std::to_string(value);
}

std::string render_value(std::int64_t value) {
  return std::to_string(value);
}

std::string render_value(scr::Digest value) {
  return value.is_null() ? "<null digest>" : value.hex();
}

std::string render_value(scr::ErrorCode value) {
  return scr::to_string(value);
}

std::string render_value(scr::Severity value) {
  return scr::to_string(value);
}

std::string render_value(scr::Modality value) {
  return scr::to_string(value);
}

std::string render_value(scr::BindingState value) {
  return scr::to_string(value);
}

std::string render_value(scr::ClassState value) {
  return scr::to_string(value);
}

std::string render_value(scr::ObligationKey value) {
  return scr::describe(value).name;
}

std::string render_value(const scr::Status& value) {
  return value.to_string();
}

std::string render_value(const scr::ClassId& value) {
  return value.str();
}

std::string render_value(const scr::RevisionRef& value) {
  return value.to_display_string();
}

std::string render_value(const scr::ClassBinding& value) {
  return value.to_string();
}

std::string render_value(const scr::BindingResolution& value) {
  return std::string(scr::to_string(value.state)) + " (" + value.detail + ")";
}

std::string render_value(const scr::Obligation& value) {
  return std::string(scr::describe(value.key()).name) + "=" + value.value_text();
}

std::string render_value(const scr::RuleViolation& value) {
  return std::string(value.rule) + " " + value.detail;
}

void remove_tree(const std::filesystem::path& root) {
  const auto exists = scr::platform::is_directory(root);
  if (!exists.ok() || !exists.value()) {
    std::error_code error;
    std::filesystem::remove(root, error);
    return;
  }
  const auto entries = scr::platform::list_files(root);
  if (entries.ok()) {
    for (const std::filesystem::path& entry : entries.value()) {
      static_cast<void>(scr::platform::remove_file(entry));
    }
  }
  const auto directories = scr::platform::list_directories(root);
  if (directories.ok()) {
    for (const std::filesystem::path& directory : directories.value()) {
      remove_tree(directory);
    }
  }
  std::error_code remove_error;
  std::filesystem::remove(root, remove_error);
}

TempDirectory::TempDirectory(const std::string& label) : path_(unique_temp_path(label)) {
  std::error_code error;
  std::filesystem::create_directories(path_, error);
}

TempDirectory::~TempDirectory() {
  remove_tree(path_);
}

scr::ClassRevision make_simple_revision(const std::string& class_id, std::uint64_t generation, std::uint64_t revision,
                                        const std::string& lineage, std::uint64_t availability_ppm) {
  const auto id = scr::ClassId::parse(class_id);
  scr::Lineage lin = scr::Lineage::genesis();
  if (lineage != "genesis") {
    const std::size_t at = lineage.find('@');
    const std::size_t dot = lineage.find('.');
    scr::RevisionRef predecessor;
    predecessor.generation = scr::Generation::from(parse_unsigned_text(lineage.substr(0, dot)));
    predecessor.revision = scr::Revision::from(
        static_cast<std::uint32_t>(parse_unsigned_text(lineage.substr(dot + 1, at - dot - 1))));
    predecessor.digest = scr::Digest::parse(lineage.substr(at + 1)).value();
    lin = scr::Lineage::from_predecessor(predecessor).value();
  }
  scr::ObligationSet obligations;
  static_cast<void>(obligations.add(scr::Obligation::create(scr::ObligationKey::AvailabilityTargetPpm,
                                                            scr::Modality::Required, availability_ppm)
                                        .value()));
  static_cast<void>(obligations.add(
      scr::Obligation::create(scr::ObligationKey::AvailabilityMaxAnnualDowntimeSeconds, scr::Modality::Required, 300)
          .value()));
  static_cast<void>(obligations.add(
      scr::Obligation::create(scr::ObligationKey::RedundancyTopology, scr::Modality::Required,
                              static_cast<std::uint64_t>(scr::RedundancyTopology::NPlusOne))
          .value()));
  static_cast<void>(obligations.add(
      scr::Obligation::create(scr::ObligationKey::RedundancyMinimumIndependentFaultDomains, scr::Modality::Required, 2)
          .value()));
  static_cast<void>(obligations.add(
      scr::Obligation::create(scr::ObligationKey::RedundancyConcurrentFaultTolerance, scr::Modality::Required, 1)
          .value()));
  scr::Metadata metadata;
  metadata.title = scr::BoundedText::create("test service class", scr::kTitleMaxBytes).value();
  // The helpers in this file are test scaffolding: if a test supplies values
  // that the library refuses, the revision is returned invalid so that the
  // failure surfaces as a refusal instead of an undefined dereference.
  const auto created = scr::make_revision(id.value(), scr::Generation::from(generation),
                                          scr::Revision::from(static_cast<std::uint32_t>(revision)), lin, obligations,
                                          {}, {}, metadata);
  return created.ok() ? created.value() : scr::ClassRevision{};
}

std::uint64_t parse_unsigned_text(const std::string& text) {
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return 0;
    }
    value = value * 10 + static_cast<std::uint64_t>(character - '0');
  }
  return value;
}

std::string revision_source(const std::string& class_id, std::uint64_t generation, std::uint64_t revision,
                            const std::string& lineage, std::uint64_t availability_ppm) {
  std::string text;
  text += "scr-class 1\n";
  text += "class " + class_id + "\n";
  text += "generation " + std::to_string(generation) + "\n";
  text += "revision " + std::to_string(revision) + "\n";
  text += "lineage " + lineage + "\n";
  text += "obligation availability.target_ppm required " + std::to_string(availability_ppm) + "\n";
  text += "obligation availability.max_annual_downtime_seconds required 300\n";
  text += "obligation redundancy.topology required n+1\n";
  text += "obligation redundancy.minimum_independent_fault_domains required 2\n";
  text += "obligation redundancy.concurrent_fault_tolerance required 1\n";
  text += "text title \"test service class\"\n";
  return text;
}

scr::ClassRevision make_published_revision(const scr::Registry& registry, const std::string& class_id,
                                           std::uint64_t availability_ppm) {
  const auto id = scr::ClassId::parse(class_id);
  const auto record = registry.class_record(id.value());
  if (!record.ok()) {
    return make_simple_revision(class_id, 1, 1, "genesis", availability_ppm);
  }
  const std::string lineage = std::to_string(record.value().generation.value()) + "." +
                              std::to_string(record.value().tip.value()) + "@" + record.value().tip_digest.hex();
  return make_simple_revision(class_id, record.value().generation.value(), record.value().tip.value() + 1, lineage,
                              availability_ppm);
}

int run_all(const std::vector<std::string>& args) {
  std::string filter;
  bool list_only = false;
  for (std::size_t index = 0; index < args.size(); ++index) {
    const std::string& argument = args[index];
    if (argument == "--filter" && index + 1 < args.size()) {
      filter = args[++index];
      continue;
    }
    if (argument == "--seed" && index + 1 < args.size()) {
      const std::string& value = args[++index];
      std::uint64_t seed = 0;
      bool valid = !value.empty();
      for (const char character : value) {
        if (character < '0' || character > '9') {
          valid = false;
          break;
        }
        seed = seed * 10 + static_cast<std::uint64_t>(character - '0');
      }
      if (valid) {
        g_seed = seed;
      }
      continue;
    }
    if (argument == "--list") {
      list_only = true;
      continue;
    }
  }

  std::vector<const TestCase*> selected;
  for (const TestCase& test : registry()) {
    if (filter.empty() || test.name.find(filter) != std::string::npos) {
      selected.push_back(&test);
    }
  }

  if (list_only) {
    for (const TestCase* test : selected) {
      std::cout << test->name << "\n";
    }
    return 0;
  }

  std::cout << "seed: " << g_seed << "\n";
  std::size_t failed = 0;
  std::size_t checks = 0;
  for (const TestCase* test : selected) {
    // The test name is printed and flushed before the test runs, so that a
    // crash inside a test identifies itself in the log even though the runtime
    // never flushes its own buffers on abnormal termination.
    std::cout << "RUN  " << test->name << std::endl;
    Context context;
    test->function(context);
    checks += context.checks;
    if (context.failures.empty()) {
      std::cout << "PASS " << test->name << " (" << context.checks << " checks)" << std::endl;
      continue;
    }
    ++failed;
    std::cout << "FAIL " << test->name << std::endl;
    for (const std::string& failure : context.failures) {
      std::cout << "     " << failure << std::endl;
    }
  }
  std::cout << (failed == 0 ? "ok" : "FAILED") << ": " << selected.size() << " test(s), " << checks
            << " check(s), " << failed << " failure(s)" << std::endl;
  return failed == 0 ? 0 : 1;
}

}  // namespace scrtest
