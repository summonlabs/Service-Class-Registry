// Entry point for the process level test executable. It shares the harness with
// the unit tests and takes the path of the scr executable under test.

#include <iostream>
#include <string>
#include <vector>

#include "harness.hpp"
#include "process_util.hpp"

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--cli" && index + 1 < argc) {
      scrtest::set_cli_path(argv[++index]);
      continue;
    }
    args.push_back(argument);
  }
  if (scrtest::cli_path().empty()) {
    std::cerr << "usage: scr_process_tests --cli <path to scr> [--filter <substring>] [--seed <n>]\n";
    return 2;
  }
  return scrtest::run_all(args);
}
