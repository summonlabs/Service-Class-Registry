# Contributing to Service Class Registry

Contributions are accepted under the Apache License, Version 2.0. There is no
Contributor License Agreement to sign: by submitting a change you agree that it
is your own work, that it is licensed to the project under the terms of LICENSE,
and that you have the right to submit it.

## What this repository accepts

Changes that keep the registry canonical, deterministic, and honest about what
it owns. In practice:

- new or corrected typed obligations, with the key descriptor updated in one
  place and the README obligation table updated in the same change;
- new cross-key consistency rules, with a stable rule identifier, a one-line
  rationale in the rule table, and a test that proves both the refusal and the
  advisory behaviour;
- durability, recovery, and hardening fixes, with a test that fails before the
  fix and passes after it;
- documentation corrections where the documentation and the implementation
  disagree. The implementation is the authority; the documentation must match.

Changes that will be refused:

- an opaque or free-form service class value that bypasses the obligation key
  registry;
- a durability path that weakens the commit protocol: no direct writes to the
  manifest, no skipping of read-back verification, no locking shortcuts;
- new third-party runtime dependencies;
- telemetry of any kind;
- test timeouts, retries that hide flakiness, or a test that reports success
  without asserting the property it names.

## Building and testing

CMake 3.20 or later and a C++20 compiler. On Windows the validated toolchain is
Visual Studio 2022 (MSVC 14.44) with the Ninja generator.

    cmake -S . -B build/rel -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/rel
    ctest --test-dir build/rel --output-on-failure

Warning policy: /W4 /WX /permissive- on MSVC, and -Wall -Wextra -Wpedantic
-Wconversion -Wsign-conversion -Werror elsewhere. Warnings are defects.

## Test expectations

Every behaviour change needs a test that would fail without it. The suites are:

- scr_tests: unit, property, and adversarial tests;
- scr_process_tests: real independent processes, covering the CLI end to end,
  writer-lock exclusion with kernel lock release after abrupt death, and crash
  consistency at every commit stage through the documented fault injection
  facility.

## Commits

Commit messages describe the change and its verification in plain language.
Do not include tool transcripts, prompts, or process instructions.
