#!/usr/bin/env bash
# Development mode: C++ server on :8080 plus the Vite dev server on :5173
# (hot reload; /api is proxied to the C++ server).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[ -x build/server/querylab_server ] || { cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4; }
build/server/querylab_server --port 8080 "$@" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null' EXIT
cd frontend
[ -d node_modules ] || npm ci --no-audit --no-fund
npm run dev
