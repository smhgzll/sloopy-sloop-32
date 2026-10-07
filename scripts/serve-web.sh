#!/usr/bin/env bash
# The SLOOP host app with the browser FM-1 panel: http://127.0.0.1:8080/ (audio on the sound card).
#   scripts/serve-web.sh [PORT] [extra sloop_host options, e.g. --audio null --bind 0.0.0.0]
# For the firmware's own web server under QEMU: scripts/run-qemu.sh, then http://127.0.0.1:18080/
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-8080}"
shift || true
"$ROOT_DIR/scripts/build-host.sh" >/dev/null
cd "$ROOT_DIR"
exec "$ROOT_DIR/build-host/sloop_host" --port "$PORT" --web "$ROOT_DIR/web" "$@"
