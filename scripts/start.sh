#!/usr/bin/env bash
# One-command startup: builds what is missing, then serves the API and the
# built frontend on http://127.0.0.1:8080.
#   scripts/start.sh                              in-memory database
#   scripts/start.sh --dataset ecommerce --scale 10000
#   scripts/start.sh --db mydata.qldb             persistent database file
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SERVER=build/server/querylab_server
if [ ! -x "$SERVER" ] || [ ! -f frontend/dist/index.html ]; then
  scripts/build.sh
fi
PORT=8080
for ((i = 1; i <= $#; i++)); do
  if [ "${!i}" = "--port" ]; then j=$((i + 1)); PORT="${!j}"; fi
done
echo "Open http://127.0.0.1:${PORT} in your browser (Ctrl+C to stop)."
exec "$SERVER" --static frontend/dist "$@"
