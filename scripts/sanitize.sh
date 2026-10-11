#!/usr/bin/env bash
# Builds a Debug engine with AddressSanitizer + UndefinedBehaviorSanitizer and
# runs the C++ test suites under them (GCC or Clang).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DQUERYLAB_SANITIZE=ON -DQUERYLAB_BUILD_SERVER=OFF
cmake --build build-asan -j"$(nproc 2>/dev/null || echo 4)"
ctest --test-dir build-asan --output-on-failure
