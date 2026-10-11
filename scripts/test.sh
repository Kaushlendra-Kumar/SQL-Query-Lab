#!/usr/bin/env bash
# Runs every test suite: C++ unit/integration tests, SQLite differential tests
# (if SQLite was found), HTTP API tests (if Python 3 was found) and the
# frontend tests.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc 2>/dev/null || echo 4)"
ctest --test-dir build --output-on-failure
if command -v npm >/dev/null 2>&1; then
  (cd frontend && { [ -d node_modules ] || npm ci --no-audit --no-fund; } && npm test)
fi
