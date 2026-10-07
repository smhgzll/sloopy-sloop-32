#!/usr/bin/env bash
# Build the ESP32-S3 firmware with the QEMU profile (null audio backend, no Wi-Fi).
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
source_idf

refresh_sdkconfig "$ROOT_DIR/build-qemu" "$ROOT_DIR/sdkconfig.defaults" "$ROOT_DIR/config/sdkconfig.qemu.defaults"
cd "$ROOT_DIR"
idf.py -B build-qemu \
  -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG="$ROOT_DIR/build-qemu/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;config/sdkconfig.qemu.defaults" \
  build
