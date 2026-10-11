#!/usr/bin/env bash
# Builds the C++ engine/server (Release) and the frontend.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc 2>/dev/null || echo 4)"
if command -v npm >/dev/null 2>&1; then
  (cd frontend && { [ -d node_modules ] || npm ci --no-audit --no-fund; } && npm run build)
else
  echo "npm not found: skipping the frontend build (the CLI and HTTP API still work)." >&2
fi
