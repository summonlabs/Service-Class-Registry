#include "harness.hpp"

#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "process_util.hpp"

using namespace scr;

namespace {

std::string json_string(const std::string& text, const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  const std::size_t start = text.find(needle);
  if (start == std::string::npos) {
    return std::string();
  }
  const std::size_t value_start = start + needle.size();
  const std::size_t end = text.find('"', value_start);
  if (end == std::string::npos) {
    return std::string();
  }
  return text.substr(value_start, end - value_start);
}

std::string json_number(const std::string& text, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t start = text.find(needle);
  if (start == std::string::npos) {
    return std::string();
  }
  std::size_t index = start + needle.size();
  std::string digits;
  while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
    digits.push_back(text[index]);
    ++index;
  }
  return digits;
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return file.good();
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return text;
}

std::string source_for(const std::string& class_id, std::uint64_t ppm, const std::string& lineage,
                       const std::string& title) {
  std::string text;
  text += "scr-class 1\n";
  text += "class " + class_id + "\n";
  text += "generation 1\n";
  text += "revision 1\n";
  text += "lineage " + lineage + "\n";
  text += "obligation availability.target_ppm required " + std::to_string(ppm) + "\n";
  text += "obligation availability.max_annual_downtime_seconds required 300\n";
  text += "obligation redundancy.topology required n+1\n";
  text += "obligation redundancy.minimum_independent_fault_tolerance required 1\n";
  text += "text title \"" + title + "\"\n";
  return text;
}

std::string valid_source(const std::string& class_id, std::uint64_t ppm, const std::string& lineage,
                         const std::string& title, std::uint64_t revision = 1) {
  std::string text;
  text += "scr-class 1\n";
  text += "class " + class_id + "\n";
  text += "generation 1\n";
  text += "revision " + std::to_string(revision) + "\n";
  text += "lineage " + lineage + "\n";
  text += "obligation availability.target_ppm required " + std::to_string(ppm) + "\n";
  text += "obligation availability.max_annual_downtime_seconds required 300\n";
  text += "obligation redundancy.topology required n+1\n";
  text += "obligation redundancy.minimum_independent_fault_domains required 2\n";
  text += "obligation redundancy.concurrent_fault_tolerance required 1\n";
  text += "text title \"" + title + "\"\n";
  return text;
}

std::vector<std::string> store_arguments(const std::filesystem::path& root) {
  return {"--store", root.string()};
}

}  // namespace

SCR_TEST(process_cli_end_to_end) {
  if (scrtest::cli_path().empty()) {
    context.fail("the scr executable path was not supplied", __FILE__, __LINE__);
    return;
  }
  scrtest::TempDirectory directory("process-cli");
  const std::filesystem::path store = directory.path() / "store";
  const std::filesystem::path source = directory.path() / "hall.scx";

  const auto initialized = scrtest::run_process(
      scrtest::cli_path(),
      {"store", "init", "--store", store.string(), "--authority", "scr-process", "--at", "1700000000", "--json"});
  SCR_REQUIRE(context, initialized.started);
  SCR_CHECK_EQ(context, initialized.exit_code, 0);
  SCR_CHECK(context, initialized.standard_output.find("\"sequence\":1") != std::string::npos);

  SCR_REQUIRE(context, write_text(source, valid_source("process.core/hall", 999000, "genesis", "process hall")));
  const auto validated = scrtest::run_process(scrtest::cli_path(),
                                              {"validate", "--file", source.string(), "--json"});
  SCR_REQUIRE(context, validated.started);
  SCR_CHECK_EQ(context, validated.exit_code, 0);
  const std::string digest = json_string(validated.standard_output, "digest");
  SCR_CHECK_EQ(context, digest.size(), std::size_t{64});

  const auto published =
      scrtest::run_process(scrtest::cli_path(),
                           {"class", "publish", "--store", store.string(), "--file", source.string(), "--at",
                            "1700000100", "--json"});
  SCR_REQUIRE(context, published.started);
  SCR_CHECK_EQ(context, published.exit_code, 0);
  SCR_CHECK_EQ(context, json_string(published.standard_output, "digest"), digest);

  // Publishing the identical file again is an idempotent success.
  const auto replay =
      scrtest::run_process(scrtest::cli_path(),
                           {"class", "publish", "--store", store.string(), "--file", source.string(), "--json"});
  SCR_REQUIRE(context, replay.started);
  SCR_CHECK_EQ(context, replay.exit_code, 0);
  SCR_CHECK(context, replay.standard_output.find("true") != std::string::npos);

  const auto shown = scrtest::run_process(
      scrtest::cli_path(), {"class", "show", "--store", store.string(), "--class", "process.core/hall", "--json"});
  SCR_REQUIRE(context, shown.started);
  SCR_CHECK_EQ(context, shown.exit_code, 0);
  SCR_CHECK_EQ(context, json_string(shown.standard_output, "tip_digest"), digest);
  SCR_CHECK_EQ(context, json_string(shown.standard_output, "state"), std::string("active"));

  const auto explained = scrtest::run_process(
      scrtest::cli_path(), {"class", "explain", "--store", store.string(), "--class", "process.core/hall"});
  SCR_REQUIRE(context, explained.started);
  SCR_CHECK_EQ(context, explained.exit_code, 0);
  SCR_CHECK(context, explained.standard_output.find("effective obligations") != std::string::npos);
  SCR_CHECK(context, explained.standard_output.find("redundancy.topology") != std::string::npos);

  const auto checked = scrtest::run_process(scrtest::cli_path(),
                                            {"check", "--store", store.string(), "--class", "process.core/hall",
                                             "--generation", "1", "--revision", "1", "--digest", digest});
  SCR_REQUIRE(context, checked.started);
  SCR_CHECK_EQ(context, checked.exit_code, 0);
  SCR_CHECK(context, checked.standard_output.find("fresh") != std::string::npos);

  // A second revision supersedes the first binding.
  const std::string second_source =
      valid_source("process.core/hall", 999900, "1.1@" + digest, "process hall", 2);
  SCR_REQUIRE(context, write_text(directory.path() / "hall2.scx", second_source));
  const auto second_publish =
      scrtest::run_process(scrtest::cli_path(),
                           {"class", "publish", "--store", store.string(), "--file",
                            (directory.path() / "hall2.scx").string(), "--at", "1700000200", "--json"});
  SCR_REQUIRE(context, second_publish.started);
  if (second_publish.exit_code != 0) {
    context.fail("publishing the second revision failed with exit code " +
                     std::to_string(second_publish.exit_code) + ": " + second_publish.standard_error +
                     second_publish.standard_output,
                 __FILE__, __LINE__);
  }

  // The same revision identity without a digest is accepted for the tip, and the
  // superseded binding is reported as stale.
  const auto stale = scrtest::run_process(scrtest::cli_path(),
                                          {"check", "--store", store.string(), "--class", "process.core/hall",
                                           "--generation", "1", "--revision", "1", "--digest", digest});
  SCR_REQUIRE(context, stale.started);
  SCR_CHECK_EQ(context, stale.exit_code, 5);
  SCR_CHECK(context, stale.standard_output.find("superseded") != std::string::npos);

  const auto compared = scrtest::run_process(scrtest::cli_path(),
                                             {"class", "compare", "--store", store.string(), "--class",
                                              "process.core/hall", "--from", "1.1", "--to", "1.2", "--json"});
  SCR_REQUIRE(context, compared.started);
  SCR_CHECK_EQ(context, compared.exit_code, 0);
  SCR_CHECK(context, compared.standard_output.find("from-ancestor-of-to") != std::string::npos);

  const auto exported = scrtest::run_process(
      scrtest::cli_path(), {"class", "export", "--store", store.string(), "--class", "process.core/hall", "--format",
                            "scx"});
  SCR_REQUIRE(context, exported.started);
  SCR_CHECK_EQ(context, exported.exit_code, 0);
  SCR_CHECK(context, exported.standard_output.find("scr-class 1") == 0);
  SCR_REQUIRE(context, write_text(directory.path() / "exported.scx", exported.standard_output));
  const auto revalidated = scrtest::run_process(
      scrtest::cli_path(), {"validate", "--file", (directory.path() / "exported.scx").string(), "--json"});
  SCR_REQUIRE(context, revalidated.started);
  SCR_CHECK_EQ(context, revalidated.exit_code, 0);

  // Retirement keeps historical bindings verifiable.
  const auto retired = scrtest::run_process(scrtest::cli_path(),
                                            {"class", "retire", "--store", store.string(), "--class",
                                             "process.core/hall", "--reason", "decommissioned for the test", "--at",
                                             "1700000300", "--json"});
  SCR_REQUIRE(context, retired.started);
  SCR_CHECK_EQ(context, retired.exit_code, 0);
  const auto after_retirement = scrtest::run_process(scrtest::cli_path(),
                                                     {"check", "--store", store.string(), "--class",
                                                      "process.core/hall", "--generation", "1", "--revision", "2",
                                                      "--digest", json_string(second_publish.standard_output, "digest")});
  SCR_REQUIRE(context, after_retirement.started);
  SCR_CHECK_EQ(context, after_retirement.exit_code, 5);
  SCR_CHECK(context, after_retirement.standard_output.find("retired") != std::string::npos);

  const auto verified = scrtest::run_process(
      scrtest::cli_path(), {"store", "verify", "--store", store.string(), "--json"});
  SCR_REQUIRE(context, verified.started);
  SCR_CHECK_EQ(context, verified.exit_code, 0);
  SCR_CHECK(context, verified.standard_output.find("\"ok\":true") != std::string::npos);

  const auto compacted = scrtest::run_process(scrtest::cli_path(),
                                              {"store", "compact", "--store", store.string(), "--json"});
  SCR_REQUIRE(context, compacted.started);
  SCR_CHECK_EQ(context, compacted.exit_code, 0);

  const auto info = scrtest::run_process(scrtest::cli_path(),
                                         {"store", "info", "--store", store.string(), "--json"});
  SCR_REQUIRE(context, info.started);
  SCR_CHECK_EQ(context, info.exit_code, 0);
  SCR_CHECK(context, info.standard_output.find("\"classes\":1") != std::string::npos);
  SCR_CHECK(context, info.standard_output.find("\"revisions\":2") != std::string::npos);
}

SCR_TEST(process_cli_usage_and_refusal_exit_codes) {
  if (scrtest::cli_path().empty()) {
    context.fail("the scr executable path was not supplied", __FILE__, __LINE__);
    return;
  }
  scrtest::TempDirectory directory("process-usage");
  const std::filesystem::path store = directory.path() / "store";
  SCR_REQUIRE(context,
              scrtest::run_process(scrtest::cli_path(),
                                   {"store", "init", "--store", store.string(), "--authority", "scr-process"})
                  .exit_code == 0);

  struct Case {
    std::vector<std::string> arguments;
    int expected;
  };
  const std::vector<Case> cases = {
      {{"nonsense"}, 2},
      {{"class", "show", "--store", store.string()}, 2},
      {{"class", "show", "--store", store.string(), "--class", "process.core/absent"}, 4},
      {{"class", "show", "--store", store.string(), "--class", "not a class"}, 2},
      {{"class", "publish", "--store", store.string(), "--file", (directory.path() / "missing.scx").string()}, 7},
      {{"class", "publish", "--store", store.string(), "--file", (directory.path() / "missing.scx").string(),
        "--unknown-option", "x"},
       2},
      {{"store", "info", "--store", (directory.path() / "absent").string()}, 6},
      {{"check", "--store", store.string(), "--class", "process.core/absent", "--generation", "1", "--revision", "1",
        "--digest", std::string(64, 'a')},
       4},
      {{"validate", "--file", (directory.path() / "missing.scx").string()}, 7},
      {{"class", "compare", "--store", store.string(), "--from", "1.1", "--to", "1.2"}, 2},
      {{"class", "compare", "--store", store.string(), "--class", "process.core/absent", "--from", "1.1", "--to",
        "1.2"},
       4},
      {{"bind", "--store", store.string(), "--class", "process.core/absent"}, 4},
  };

  for (const Case& test_case : cases) {
    const scrtest::ProcessResult result = scrtest::run_process(scrtest::cli_path(), test_case.arguments);
    SCR_REQUIRE(context, result.started);
    if (result.exit_code != test_case.expected) {
      context.fail("unexpected exit code " + std::to_string(result.exit_code) + " (expected " +
                       std::to_string(test_case.expected) + ") for: " + test_case.arguments.front() + " -> " +
                       result.standard_output,
                   __FILE__, __LINE__);
    }
  }

  // A contradictory class is refused with exit code 3 and a rule identifier.
  std::string contradictory = valid_source("process.core/contradiction", 999000, "genesis", "contradiction");
  const std::string tolerance_line = "obligation redundancy.concurrent_fault_tolerance required 1\n";
  const std::size_t tolerance_at = contradictory.find(tolerance_line);
  SCR_REQUIRE(context, tolerance_at != std::string::npos);
  contradictory.replace(tolerance_at, tolerance_line.size(),
                        "obligation redundancy.concurrent_fault_tolerance required 0\n");
  contradictory += "obligation maintenance.mode required online\n";
  const std::filesystem::path contradictory_path = directory.path() / "contradiction.scx";
  SCR_REQUIRE(context, write_text(contradictory_path, contradictory));
  const auto refused = scrtest::run_process(scrtest::cli_path(),
                                            {"class", "publish", "--store", store.string(), "--file",
                                             contradictory_path.string()});
  SCR_REQUIRE(context, refused.started);
  SCR_CHECK_EQ(context, refused.exit_code, 3);
}

SCR_TEST(process_writer_exclusion_and_kernel_release) {
  if (scrtest::cli_path().empty()) {
    context.fail("the scr executable path was not supplied", __FILE__, __LINE__);
    return;
  }
  scrtest::TempDirectory directory("process-lock");
  const std::filesystem::path store = directory.path() / "store";
  SCR_REQUIRE(context,
              scrtest::run_process(scrtest::cli_path(),
                                   {"store", "init", "--store", store.string(), "--authority", "scr-process"})
                  .exit_code == 0);

  scrtest::ChildProcess holder;
  std::string error;
  SCR_REQUIRE(context, holder.start(scrtest::cli_path(),
                                    {"store", "lock-probe", "--store", store.string(), "--hold-ms", "60000"}, {}, &error));
  const std::string line = holder.read_line();
  SCR_CHECK(context, line.find("lock acquired") != std::string::npos);

  // While the exclusive writer is alive, every other writer is refused and even
  // a read-only open is refused, because the reader lock conflicts with the
  // writer's exclusive handle.
  const auto blocked = scrtest::run_process(scrtest::cli_path(),
                                            {"store", "info", "--store", store.string()});
  SCR_REQUIRE(context, blocked.started);
  SCR_CHECK_EQ(context, blocked.exit_code, 6);
  if (blocked.standard_output.find("StoreBusy") == std::string::npos) {
    context.fail("the blocked reader did not report StoreBusy: exit " + std::to_string(blocked.exit_code) +
                     " output: " + blocked.standard_output,
                 __FILE__, __LINE__);
  }

  const std::filesystem::path source = directory.path() / "hall.scx";
  SCR_REQUIRE(context, write_text(source, valid_source("process.core/lock", 999000, "genesis", "lock")));
  const auto publish_blocked = scrtest::run_process(
      scrtest::cli_path(), {"class", "publish", "--store", store.string(), "--file", source.string()});
  SCR_REQUIRE(context, publish_blocked.started);
  SCR_CHECK_EQ(context, publish_blocked.exit_code, 6);

  // Abrupt death of the holder releases the kernel lock.
  holder.kill();
  int exit_code = -1;
  SCR_REQUIRE(context, holder.wait_for_exit(&exit_code));
  SCR_CHECK(context, exit_code != 0);

  const auto released = scrtest::run_process(scrtest::cli_path(),
                                             {"store", "info", "--store", store.string()});
  SCR_REQUIRE(context, released.started);
  SCR_CHECK_EQ(context, released.exit_code, 0);
  const auto publish_after = scrtest::run_process(
      scrtest::cli_path(), {"class", "publish", "--store", store.string(), "--file", source.string()});
  SCR_REQUIRE(context, publish_after.started);
  SCR_CHECK_EQ(context, publish_after.exit_code, 0);
}

SCR_TEST(process_crash_at_every_commit_stage) {
  if (scrtest::cli_path().empty()) {
    context.fail("the scr executable path was not supplied", __FILE__, __LINE__);
    return;
  }
  scrtest::TempDirectory directory("process-crash");
  const std::vector<std::string> stages = {"records-staged",      "records-published", "manifest-staged",
                                           "manifest-published",  "guard-staged",      "guard-published"};

  for (const std::string& stage : stages) {
    const std::filesystem::path store = directory.path() / ("store-" + stage);
    SCR_REQUIRE(context,
                scrtest::run_process(scrtest::cli_path(),
                                     {"store", "init", "--store", store.string(), "--authority", "scr-process"})
                    .exit_code == 0);

    const std::filesystem::path source = directory.path() / ("hall-" + stage + ".scx");
    SCR_REQUIRE(context, write_text(source, valid_source("crash.core/hall", 999000, "genesis", "crash hall")));
    const auto validated = scrtest::run_process(scrtest::cli_path(), {"validate", "--file", source.string(), "--json"});
    SCR_REQUIRE(context, validated.exit_code == 0);
    const std::string digest = json_string(validated.standard_output, "digest");
    SCR_REQUIRE(context, digest.size() == std::size_t{64});

    // Abrupt death at a named commit stage.
    const auto crashed = scrtest::run_process(
        scrtest::cli_path(),
        {"class", "publish", "--store", store.string(), "--file", source.string()},
        {{"SCR_FAULT_INJECT", stage}});
    SCR_REQUIRE(context, crashed.started);
    SCR_CHECK_EQ(context, crashed.exit_code, 137);

    // The store must still verify: the crash is either fully published or
    // completely invisible.
    const auto verified = scrtest::run_process(scrtest::cli_path(),
                                               {"store", "verify", "--store", store.string(), "--json"});
    SCR_REQUIRE(context, verified.started);
    SCR_CHECK_EQ(context, verified.exit_code, 0);
    SCR_CHECK(context, verified.standard_output.find("\"ok\":true") != std::string::npos);

    // The class is either absent or exactly the crashed revision.
    const auto shown = scrtest::run_process(
        scrtest::cli_path(), {"class", "show", "--store", store.string(), "--class", "crash.core/hall", "--json"});
    SCR_REQUIRE(context, shown.started);
    if (shown.exit_code == 0) {
      SCR_CHECK_EQ(context, json_string(shown.standard_output, "tip_digest"), digest);
      SCR_CHECK_EQ(context, json_number(shown.standard_output, "tip"), std::string("1"));
    } else {
      SCR_CHECK_EQ(context, shown.exit_code, 4);
    }

    // Retrying the same publication completes it, as a fresh publication or as
    // an idempotent replay of the revision the crash already published.
    const auto retried = scrtest::run_process(
        scrtest::cli_path(), {"class", "publish", "--store", store.string(), "--file", source.string(), "--json"});
    SCR_REQUIRE(context, retried.started);
    SCR_CHECK_EQ(context, retried.exit_code, 0);
    SCR_CHECK_EQ(context, json_string(retried.standard_output, "digest"), digest);

    const auto final_show = scrtest::run_process(
        scrtest::cli_path(), {"class", "show", "--store", store.string(), "--class", "crash.core/hall", "--json"});
    SCR_REQUIRE(context, final_show.exit_code == 0);
    SCR_CHECK_EQ(context, json_string(final_show.standard_output, "tip_digest"), digest);
    SCR_CHECK(context, final_show.standard_output.find("\"history\":[{") != std::string::npos);
    SCR_CHECK_EQ(context, json_string(final_show.standard_output, "state"), std::string("active"));

    // A second identical publication is an idempotent no-op.
    const auto replay = scrtest::run_process(
        scrtest::cli_path(), {"class", "publish", "--store", store.string(), "--file", source.string(), "--json"});
    SCR_REQUIRE(context, replay.exit_code == 0);
    SCR_CHECK(context, replay.standard_output.find("\"idempotent\":true") != std::string::npos);

    const auto final_verify = scrtest::run_process(scrtest::cli_path(),
                                                   {"store", "verify", "--store", store.string(), "--json"});
    SCR_REQUIRE(context, final_verify.exit_code == 0);
    SCR_CHECK(context, final_verify.standard_output.find("\"ok\":true") != std::string::npos);
  }
}
