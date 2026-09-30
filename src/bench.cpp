// Benchmark harness for completed operations only.
//
// Every measurement times a completed operation: a durable publish includes
// staging, flush, read-back verification, and manifest plus guard publication,
// and a store open includes full manifest, guard, and record verification. No
// measurement reports enqueue or submission latency as completed work.
//
// Provenance labels: REAL means the measurement includes the real durable or
// operating-system path; SYNTHETIC means the measurement is in-memory or pure
// computation with no device interaction. All numbers are single-host
// measurements of this build on this machine and are not a comparison.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "scr/platform.hpp"
#include "scr/registry.hpp"
#include "scr/report.hpp"
#include "scr/store.hpp"
#include "scr/text.hpp"
#include "scr/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
  std::string name;
  std::string provenance;
  std::size_t iterations = 0;
  double total_seconds = 0.0;
};

std::string build_label() {
#ifdef NDEBUG
  return "Release";
#else
  return "Debug";
#endif
}

std::string platform_label() {
#if defined(_WIN32)
#if defined(_M_X64) || defined(__x86_64__)
  return "Windows x64";
#else
  return "Windows";
#endif
#elif defined(__linux__)
  return "Linux";
#elif defined(__APPLE__)
  return "macOS";
#else
  return "unknown";
#endif
}

std::string scx_source(const std::string& class_id, std::uint64_t generation, std::uint64_t revision,
                       const std::string& lineage, std::uint64_t availability_ppm, std::uint64_t domains,
                       std::uint64_t tolerance) {
  std::string text;
  text += "scr-class 1\n";
  text += "class " + class_id + "\n";
  text += "generation " + std::to_string(generation) + "\n";
  text += "revision " + std::to_string(revision) + "\n";
  text += "lineage " + lineage + "\n";
  text += "obligation availability.target_ppm required " + std::to_string(availability_ppm) + "\n";
  text += "obligation availability.max_annual_downtime_seconds required 300\n";
  text += "obligation redundancy.topology required n+1\n";
  text += "obligation redundancy.minimum_independent_fault_domains required " + std::to_string(domains) + "\n";
  text += "obligation redundancy.concurrent_fault_tolerance required " + std::to_string(tolerance) + "\n";
  text += "obligation redundancy.failover_mode required automatic\n";
  text += "obligation redundancy.maximum_failover_seconds required 60\n";
  text += "obligation maintenance.mode required concurrent-maintainable\n";
  text += "obligation power.feed_count required 2\n";
  text += "obligation power.path_independence required dual-independent\n";
  text += "obligation power.transfer_mode required automatic\n";
  text += "obligation power.maximum_transfer_seconds required 20\n";
  text += "obligation cooling.mode required redundant-mechanical\n";
  text += "obligation cooling.independent_paths required 2\n";
  text += "obligation recovery.time_objective_seconds required 120\n";
  text += "obligation recovery.restore_mode required warm\n";
  text += "obligation operations.monitoring_interval_seconds required 30\n";
  text += "text title \"benchmark service class\"\n";
  return text;
}

std::uint64_t total_bytes(const std::filesystem::path& root) {
  std::uint64_t total = 0;
  std::error_code error;
  for (const std::filesystem::recursive_directory_iterator::value_type& entry :
       std::filesystem::recursive_directory_iterator(root, error)) {
    std::error_code size_error;
    const auto size = entry.file_size(size_error);
    if (!size_error) {
      total += static_cast<std::uint64_t>(size);
    }
  }
  return total;
}

void print_measurement(std::ostream& out, const Measurement& measurement, bool json, bool first) {
  const double per_op_seconds =
      measurement.iterations == 0 ? 0.0 : measurement.total_seconds / static_cast<double>(measurement.iterations);
  const double ns_per_op = per_op_seconds * 1e9;
  const double ops_per_second = per_op_seconds > 0.0 ? 1.0 / per_op_seconds : 0.0;
  if (json) {
    if (!first) {
      out << ",\n";
    }
    out << "  {\"name\":\"" << measurement.name << "\",\"provenance\":\"" << measurement.provenance
        << "\",\"iterations\":" << measurement.iterations << ",\"total_seconds\":" << std::fixed
        << measurement.total_seconds << ",\"ns_per_operation\":" << ns_per_op
        << ",\"operations_per_second\":" << ops_per_second << "}";
    return;
  }
  char line[256];
  std::snprintf(line, sizeof(line), "  %-22s %-10s %8zu  %10.3f ms  %12.1f ns/op  %12.1f ops/s", measurement.name.c_str(),
                measurement.provenance.c_str(), measurement.iterations, measurement.total_seconds * 1e3, ns_per_op,
                ops_per_second);
  out << line << "\n";
}

}  // namespace

int run_bench(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  std::size_t iterations = 200;
  bool json = false;
  for (std::size_t index = 0; index < args.size(); ++index) {
    const std::string& token = args[index];
    if (token == "--json") {
      json = true;
      continue;
    }
    if (token == "--iterations") {
      if (index + 1 >= args.size()) {
        err << "scr: error: --iterations requires a value\n";
        return 2;
      }
      const std::string value = args[++index];
      std::size_t parsed = 0;
      for (const char character : value) {
        if (character < '0' || character > '9') {
          err << "scr: error: --iterations must be a non-negative integer\n";
          return 2;
        }
        parsed = parsed * 10 + static_cast<std::size_t>(character - '0');
        if (parsed > 100000) {
          err << "scr: error: --iterations is limited to 100000\n";
          return 2;
        }
      }
      if (parsed == 0) {
        err << "scr: error: --iterations must be at least 1\n";
        return 2;
      }
      iterations = parsed;
      continue;
    }
    err << "scr: error: unknown bench option '" << token << "'\n";
    return 2;
  }

  std::error_code error;
  const std::filesystem::path temp_root = std::filesystem::temp_directory_path(error);
  if (error) {
    err << "scr: error: the temporary directory is not available: " << error.message() << "\n";
    return 7;
  }
  const std::filesystem::path bench_root =
      temp_root / ("scr-bench-" + scr::platform::process_id_text() + "-" + std::to_string(iterations));

  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{bench_root};

  const auto authority = scr::AuthorityId::parse("scr-bench");
  if (!authority.ok()) {
    err << "scr: error: internal benchmark authority is invalid\n";
    return 7;
  }
  scr::Timestamp at = scr::Timestamp::unset();
  {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    const auto stamp = scr::Timestamp::from_unix_seconds(seconds);
    if (stamp.ok()) {
      at = stamp.value();
    }
  }

  auto store = scr::Store::create(bench_root, authority.value(), scr::Epoch::from(1), at);
  if (!store.ok()) {
    err << "scr: error: cannot create the benchmark store: " << store.status().to_string() << "\n";
    return 7;
  }

  std::vector<Measurement> measurements;

  // Publish a base class and one composition parent so that the later
  // measurements exercise composition instead of a trivial single revision.
  auto publish_source = [&](const std::string& class_id, const std::string& lineage, std::uint64_t generation,
                            std::uint64_t revision) -> bool {
    const std::string source = scx_source(class_id, generation, revision, lineage, 999500, 2, 1);
    const auto parsed = scr::parse_scx(source);
    if (!parsed.ok()) {
      err << "scr: error: the benchmark source did not parse: " << parsed.status().to_string() << "\n";
      return false;
    }
    scr::PublishOptions options;
    options.at = at;
    options.allow_new_generation = generation > 1;
    const auto outcome = store.value().publish(parsed.value(), options);
    if (!outcome.ok()) {
      err << "scr: error: the benchmark publication was refused: " << outcome.status().to_string() << "\n";
      return false;
    }
    return true;
  };

  if (!publish_source("bench.facility/power-train", "genesis", 1, 1)) {
    return 3;
  }
  const auto power_class = scr::ClassId::parse("bench.facility/power-train");
  if (!power_class.ok()) {
    return 7;
  }
  const auto power_tip = store.value().registry().class_record(power_class.value());
  if (!power_tip.ok()) {
    err << "scr: error: the benchmark parent class is missing\n";
    return 7;
  }
  const std::string parent_lineage = "1.1@" + power_tip.value().tip_digest.hex();

  // A composed class whose parent chain is exercised by flatten and explain.
  {
    std::string source = scx_source("bench.facility/dc-hall", 1, 1, "genesis", 999900, 2, 1);
    source += "parent bench.facility/power-train @ " + parent_lineage + "\n";
    const auto parsed = scr::parse_scx(source);
    if (!parsed.ok()) {
      err << "scr: error: the composed benchmark source did not parse: " << parsed.status().to_string() << "\n";
      return 3;
    }
    scr::PublishOptions options;
    options.at = at;
    const auto outcome = store.value().publish(parsed.value(), options);
    if (!outcome.ok()) {
      err << "scr: error: the composed benchmark publication was refused: " << outcome.status().to_string() << "\n";
      return 3;
    }
  }

  const auto class_id = scr::ClassId::parse("bench.facility/dc-hall");
  if (!class_id.ok()) {
    return 7;
  }
  const auto chain_class = scr::ClassId::parse("bench.facility/chain");
  if (!chain_class.ok()) {
    return 7;
  }

  // 1. Canonical encoding and content digest (SYNTHETIC: pure computation).
  {
    const auto parsed = scr::parse_scx(scx_source("bench.facility/dc-hall", 1, 1, "genesis", 999900, 2, 1));
    if (!parsed.ok()) {
      return 7;
    }
    const auto start = Clock::now();
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < iterations; ++index) {
      const auto frame = scr::encode_revision_frame(parsed.value());
      if (!frame.ok()) {
        return 7;
      }
      const auto digest = scr::Digest::of(frame.value().data(), frame.value().size());
      sink ^= digest.bytes()[0];
    }
    const auto finish = Clock::now();
    if (sink == 0xFFFFFFFFull) {
      out << "";
    }
    measurements.push_back(Measurement{"canonical_digest", "SYNTHETIC", iterations,
                                       std::chrono::duration<double>(finish - start).count()});
  }

  // 2. Fresh binding resolution against the authoritative index (SYNTHETIC).
  {
    const auto record = store.value().registry().class_record(class_id.value());
    if (!record.ok()) {
      return 7;
    }
    scr::ClassBinding binding;
    binding.class_id = class_id.value();
    binding.revision.generation = record.value().generation;
    binding.revision.revision = record.value().tip;
    binding.revision.digest = record.value().tip_digest;
    const auto start = Clock::now();
    for (std::size_t index = 0; index < iterations; ++index) {
      const auto resolution = store.value().registry().resolve(binding);
      if (!resolution.ok() || resolution.value().state != scr::BindingState::Fresh) {
        err << "scr: error: the benchmark binding did not resolve fresh\n";
        return 7;
      }
    }
    const auto finish = Clock::now();
    measurements.push_back(Measurement{"resolve_fresh_binding", "SYNTHETIC", iterations,
                                       std::chrono::duration<double>(finish - start).count()});
  }

  // 3. Flattening and explanation of the authoritative revision (SYNTHETIC).
  {
    const auto start = Clock::now();
    for (std::size_t index = 0; index < iterations; ++index) {
      const auto explanation = store.value().registry().explain(class_id.value());
      if (!explanation.ok()) {
        err << "scr: error: the benchmark explanation failed\n";
        return 7;
      }
    }
    const auto finish = Clock::now();
    measurements.push_back(Measurement{"explain_authoritative", "SYNTHETIC", iterations,
                                       std::chrono::duration<double>(finish - start).count()});
  }

  // 4. Durable publication of a new revision: staging, flush, read-back, and
  //    manifest plus guard publication (REAL: the full durable path).
  {
    const std::uint64_t bytes_before = total_bytes(bench_root);
    const auto start = Clock::now();
    std::size_t completed = 0;
    for (std::size_t index = 0; index < iterations; ++index) {
      std::string lineage = "genesis";
      const auto record = store.value().registry().class_record(chain_class.value());
      if (record.ok()) {
        lineage = std::to_string(record.value().generation.value()) + "." +
                  std::to_string(record.value().tip.value()) + "@" + record.value().tip_digest.hex();
      }
      const std::string source =
          scx_source("bench.facility/chain", 1, static_cast<std::uint64_t>(index) + 1, lineage, 999000, 2, 1);
      const auto revised = scr::parse_scx(source);
      if (!revised.ok()) {
        err << "scr: error: a benchmark revision did not parse: " << revised.status().to_string() << "\n";
        return 7;
      }
      scr::PublishOptions options;
      options.at = at;
      const auto outcome = store.value().publish(revised.value(), options);
      if (!outcome.ok()) {
        err << "scr: error: a benchmark publication was refused: " << outcome.status().to_string() << "\n";
        return 3;
      }
      ++completed;
    }
    const auto finish = Clock::now();
    const std::uint64_t bytes_after = total_bytes(bench_root);
    const double seconds = std::chrono::duration<double>(finish - start).count();
    measurements.push_back(Measurement{"publish_durable", "REAL", completed, seconds});
    if (!json) {
      const double per_op = completed == 0 ? 0.0 : static_cast<double>(bytes_after - bytes_before) / completed;
      out << "  (durable publish wrote " << per_op << " bytes per completed operation on average)\n";
    }
  }

  // The exclusive writer lock is held for the lifetime of the Store object, so
  // it is released before the cold-open measurement.
  const std::size_t classes_after = store.value().info().class_count;
  const std::size_t revisions_after = store.value().info().revision_count;
  const std::string manifest_after = store.value().info().manifest_digest.hex();
  store.value() = scr::Store{};

  // 5. Verified cold open of the store (REAL: manifest, guard, and record
  //    verification on every open).
  {
    const std::size_t open_iterations = std::min<std::size_t>(iterations, 20);
    const auto start = Clock::now();
    for (std::size_t index = 0; index < open_iterations; ++index) {
      auto reopened = scr::Store::open(bench_root, scr::StoreMode::ReadOnly);
      if (!reopened.ok()) {
        err << "scr: error: the benchmark reopen failed: " << reopened.status().to_string() << "\n";
        return 7;
      }
    }
    const auto finish = Clock::now();
    measurements.push_back(Measurement{"open_verified_cold", "REAL", open_iterations,
                                       std::chrono::duration<double>(finish - start).count()});
  }

  if (json) {
    out << "{\n";
    out << "  \"build\":\"" << build_label() << "\",\"platform\":\"" << platform_label()
        << "\",\"iterations\":" << iterations << ",\n  \"measurements\":[\n";
    bool first = true;
    for (const Measurement& measurement : measurements) {
      print_measurement(out, measurement, true, first);
      first = false;
    }
    out << "\n  ]\n}\n";
    return 0;
  }

  out << scr::kProductName << " " << scr::kVersionString << " benchmark\n";
  out << "build: " << build_label() << ", platform: " << platform_label() << ", iterations: " << iterations << "\n";
  out << "methodology: completed operations only; REAL includes the durable path with flush, read-back, and\n";
  out << "publication; SYNTHETIC is in-memory or pure computation. Single-host measurement; absolute values\n";
  out << "depend on this machine and are not a comparison against any other build.\n";
  out << "  operation              provenance  iters          total          per op        rate\n";
  for (const Measurement& measurement : measurements) {
    print_measurement(out, measurement, false, true);
  }
  out << "store after the run: " << classes_after << " classes, " << revisions_after << " revisions, "
      << manifest_after << "\n";
  return 0;
}
