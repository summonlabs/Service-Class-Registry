#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "scr/registry.hpp"
#include "scr/store.hpp"

namespace scrtest {

// Minimal first-party test harness: no third-party dependency, deterministic
// ordering, and explicit failure messages that name the file and line.

// Value rendering for failure messages. Declared before Context so that the
// check_equal template can call them.
std::string render_value(bool value);
std::string render_value(const char* value);
std::string render_value(const std::string& value);
std::string render_value(std::string_view value);
std::string render_value(int value);
std::string render_value(unsigned int value);
std::string render_value(std::uint64_t value);
std::string render_value(std::int64_t value);
std::string render_value(scr::Digest value);
std::string render_value(scr::ErrorCode value);
std::string render_value(scr::Severity value);
std::string render_value(scr::Modality value);
std::string render_value(scr::BindingState value);
std::string render_value(scr::ClassState value);
std::string render_value(scr::ObligationKey value);
std::string render_value(const scr::Status& value);
std::string render_value(const scr::ClassId& value);
std::string render_value(const scr::RevisionRef& value);
std::string render_value(const scr::ClassBinding& value);
std::string render_value(const scr::BindingResolution& value);
std::string render_value(const scr::Obligation& value);
std::string render_value(const scr::RuleViolation& value);

// Fallback for types that have no dedicated renderer. Arrays and pointers are
// excluded so that string literals keep resolving to the const char* overload
// instead of becoming ambiguous. The name deliberately differs from
// scr::describe so that argument dependent lookup cannot make a call ambiguous.
template <typename T, typename std::enable_if_t<!std::is_array_v<T> && !std::is_pointer_v<T>, int> = 0>
std::string render_value(const T&) {
  return "<value>";
}

struct Context {
  std::vector<std::string> failures;
  std::size_t checks = 0;

  void note_failure(const std::string& message, const char* file, int line);
  bool check(bool condition, const char* expression, const char* file, int line);
  void fail(const std::string& message, const char* file, int line);

  template <typename A, typename B>
  bool check_equal(const A& actual, const B& expected, const char* actual_text, const char* expected_text,
                   const char* file, int line) {
    ++checks;
    if (actual == expected) {
      return true;
    }
    std::ostringstream message;
    message << actual_text << " == " << expected_text << " failed: actual " << render_value(actual)
            << ", expected " << render_value(expected);
    note_failure(message.str(), file, line);
    return false;
  }
};

using TestFunction = void (*)(Context&);

struct TestCase {
  std::string name;
  TestFunction function;
};

void register_test(const std::string& name, TestFunction function);
int run_all(const std::vector<std::string>& args);
// Seed used by the property tests; printed by the runner so a failure can be
// reproduced exactly with --seed <n>.
std::uint64_t current_seed();

// Deterministic pseudo random generator (splitmix64). Property tests print the
// seed they used so that a failure can be reproduced exactly.
class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
  }

  std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : (next() % bound); }

  bool chance(std::uint64_t numerator, std::uint64_t denominator) {
    return below(denominator) < numerator;
  }

 private:
  std::uint64_t state_;
};

// Temporary directory that removes itself: tests never leave residue behind.
class TempDirectory {
 public:
  explicit TempDirectory(const std::string& label);
  ~TempDirectory();

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// Recursive removal that works for deep paths on Windows.
void remove_tree(const std::filesystem::path& root);

// Helpers used by several suites.
scr::ClassRevision make_simple_revision(const std::string& class_id, std::uint64_t generation, std::uint64_t revision,
                                        const std::string& lineage, std::uint64_t availability_ppm);
scr::ClassRevision make_published_revision(const scr::Registry& registry, const std::string& class_id,
                                           std::uint64_t availability_ppm);
std::string revision_source(const std::string& class_id, std::uint64_t generation, std::uint64_t revision,
                            const std::string& lineage, std::uint64_t availability_ppm);
// Strict decimal parsing helper used by the test builders.
std::uint64_t parse_unsigned_text(const std::string& text);

}  // namespace scrtest

#define SCR_TEST(name)                                                                        \
  static void name(::scrtest::Context& context);                                              \
  static const bool scrtest_registered_##name =                                               \
      (::scrtest::register_test(#name, &name), true);                                         \
  static void name(::scrtest::Context& context)

#define SCR_CHECK(ctx, ...) (ctx).check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)

#define SCR_CHECK_EQ(ctx, actual, expected)                                                       \
  (ctx).check_equal((actual), (expected), #actual, #expected, __FILE__, __LINE__)

#define SCR_REQUIRE(ctx, ...)                                                                    \
  do {                                                                                           \
    if (!(ctx).check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)) {                          \
      (ctx).fail("precondition failed; the rest of this test cannot run", __FILE__, __LINE__);     \
      return;                                                                                    \
    }                                                                                            \
  } while (false)