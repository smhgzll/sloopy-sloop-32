#!/usr/bin/env bash
# Which ESP32-S3 GPIO does a wire reach (touch it with GND)? scripts/probe-pins.sh [/dev/ttyACM0]
# [--seconds 180]. Holds the chip in its download mode (no firmware runs, no pin is driven).
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
source_idf >/dev/null
load_local_env
if [[ $# -eq 0 || "$1" == --* ]]; then set -- "${SLOOPY_SERIAL_PORT:-/dev/ttyACM0}" "$@"; fi
exec python "$ROOT_DIR/scripts/probe_pins.py" "$@"
