#!/usr/bin/env bash
# Builds the engine for the browser (needs Emscripten: emcmake/emcc on PATH)
# and copies querylab.js + querylab.wasm into frontend/public/wasm, where the
# frontend's Web Worker loads them. Without emsdk installed, use Docker:
#   docker run --rm -v "$PWD":/src -w /src emscripten/emsdk:4.0.10 scripts/build-wasm.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release -DQUERYLAB_BUILD_TESTS=OFF
cmake --build build-wasm -j"$(nproc 2>/dev/null || echo 4)"
mkdir -p frontend/public/wasm
cp build-wasm/wasm/querylab.js build-wasm/wasm/querylab.wasm frontend/public/wasm/
