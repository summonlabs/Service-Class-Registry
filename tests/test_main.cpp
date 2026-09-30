// Entry point for the unit test executable. The harness itself lives in
// harness.cpp so that the process test executable can share it.

#include <string>
#include <vector>

#include "harness.hpp"

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int index = 1; index < argc; ++index) {
    args.push_back(argv[index]);
  }
  return scrtest::run_all(args);
}
