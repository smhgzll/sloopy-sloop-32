#!/usr/bin/env bash
# Run the QEMU-profile firmware in Espressif's ESP32-S3 QEMU: 16 MB flash, 16 MB PSRAM (quad: KI-5).
#   scripts/run-qemu.sh                 interactive console (Ctrl-A X quits)
#   KEEP_FLASH=1 scripts/run-qemu.sh    keep build-qemu/flash_image.bin from the last run (the
#                                       "sloop" store and NVS survive, like a real board)
#   QEMU_LOG=file scripts/run-qemu.sh   non-interactive: console to file, stdin from /dev/null
#   QEMU_WEB_PORT=18080 (default)       the firmware's web panel at http://127.0.0.1:18080/
#                                       (QEMU user-mode network, OpenCores Ethernet NIC)
#   QEMU_TCG_THREAD=multi (default)     one host thread per emulated core; "single": both on one
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
source_idf

BUILD_DIR="$ROOT_DIR/build-qemu"
if [[ ! -f "$BUILD_DIR/flash_args" ]]; then
  "$ROOT_DIR/scripts/build-qemu.sh"
fi
cd "$BUILD_DIR"
if [[ "${KEEP_FLASH:-0}" != 1 || ! -f flash_image.bin ]]; then
  python -m esptool --chip esp32s3 merge_bin --fill-flash-size 16MB -o flash_image.bin @flash_args >/dev/null
fi
QEMU=(qemu-system-xtensa -nographic -machine esp32s3 -m 16M -accel "tcg,thread=${QEMU_TCG_THREAD:-multi}"
      -drive file=flash_image.bin,if=mtd,format=raw
      -nic "user,model=open_eth,hostfwd=tcp:127.0.0.1:${QEMU_WEB_PORT:-18080}-:80")
if [[ -n "${QEMU_LOG:-}" ]]; then
  # QEMU crashes with a non-terminal stdin (KNOWN_ISSUES KI-1)
  exec "${QEMU[@]}" < /dev/null > "$QEMU_LOG" 2>&1
fi
exec "${QEMU[@]}"
