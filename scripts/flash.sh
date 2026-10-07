#!/usr/bin/env bash
# Build (if needed) and flash the hardware firmware: scripts/flash.sh [/dev/ttyACM0]
# The port comes from the argument, SLOOPY_SERIAL_PORT in config/local.env, or /dev/ttyACM0.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
load_local_env
PORT="${1:-${SLOOPY_SERIAL_PORT:-/dev/ttyACM0}}"
"$ROOT_DIR/scripts/build-hardware.sh"
source_idf
[[ -e "$PORT" ]] || { echo "No serial device $PORT (board connected? see scripts/doctor.sh)" >&2; exit 1; }
cd "$ROOT_DIR"
idf.py -B build-hw -p "$PORT" flash
