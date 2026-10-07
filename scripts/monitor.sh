#!/usr/bin/env bash
# Serial console of the hardware firmware (Ctrl-] quits): scripts/monitor.sh [/dev/ttyACM0]
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
source_idf
load_local_env
PORT="${1:-${SLOOPY_SERIAL_PORT:-/dev/ttyACM0}}"
cd "$ROOT_DIR"
idf.py -B build-hw -p "$PORT" monitor
