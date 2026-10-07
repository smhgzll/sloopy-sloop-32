#!/usr/bin/env bash
# menuconfig for the hardware build (build-hw/sdkconfig). Note: settings you want to keep belong
# in config/local.env (pins, Wi-Fi) or the tracked sdkconfig defaults: build-hw/sdkconfig is
# regenerated when those change.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
source_idf
"$ROOT_DIR/scripts/generate-local-sdkconfig.sh"
cd "$ROOT_DIR"
idf.py -B build-hw -DIDF_TARGET=esp32s3 -DSDKCONFIG="$ROOT_DIR/build-hw/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;config/sdkconfig.hardware.defaults;config/sdkconfig.local.defaults" \
  menuconfig
