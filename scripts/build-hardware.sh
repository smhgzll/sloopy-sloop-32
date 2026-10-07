#!/usr/bin/env bash
# Build the real-hardware firmware (octal PSRAM, I2S to the PCM5102A, Wi-Fi + web panel) into
# build-hw/. Pins and Wi-Fi come from config/local.env (-> config/sdkconfig.local.defaults).
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
source_idf
"$ROOT_DIR/scripts/generate-local-sdkconfig.sh"

refresh_sdkconfig "$ROOT_DIR/build-hw" "$ROOT_DIR/sdkconfig.defaults" "$ROOT_DIR/config/sdkconfig.hardware.defaults" "$ROOT_DIR/config/sdkconfig.local.defaults"
cd "$ROOT_DIR"
idf.py -B build-hw \
  -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG="$ROOT_DIR/build-hw/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;config/sdkconfig.hardware.defaults;config/sdkconfig.local.defaults" \
  build
