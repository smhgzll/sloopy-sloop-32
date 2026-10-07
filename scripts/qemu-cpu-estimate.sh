#!/usr/bin/env bash
# Estimate SLOOP's audio CPU cost on the ESP32-S3 before having the board.
# QEMU with -icount shift=0 advances its virtual clock by exactly 1 ns per guest instruction, so the
# firmware's esp_timer measurement of the self-test render (4 s of a 4-track song, before boot)
# is an instruction count. Cycles = instructions x CPI; the CPI of the real LX7 (cache misses, PSRAM)
# is NOT known here: the estimate brackets CPI 1.0..1.5. Real numbers: hardware test H-7.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
source_idf
"$ROOT_DIR/scripts/build-qemu.sh" >/dev/null || { echo "build failed" >&2; exit 1; }
cd "$ROOT_DIR/build-qemu"
IMG="$ROOT_DIR/build-qemu/icount_image.bin"
LOG="$ROOT_DIR/build-qemu/qemu-icount.log"
python -m esptool --chip esp32s3 merge_bin --fill-flash-size 16MB -o "$IMG" @flash_args >/dev/null
qemu-system-xtensa -nographic -machine esp32s3 -m 16M -icount shift=0 \
  -drive file="$IMG",if=mtd,format=raw < /dev/null > "$LOG" 2>&1 &
QPID=$!
for _ in $(seq 1 300); do sleep 1; grep -q SLOOP_SELFTEST_RENDER "$LOG" && break; done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
line="$(grep -m1 SLOOP_SELFTEST_RENDER "$LOG")" || { echo "no render line in $LOG" >&2; exit 1; }
echo "$line"
python3 - "$line" <<'PY'
import re, sys
m = dict(re.findall(r"(\w+)=(\S+)", sys.argv[1]))
audio_s = int(m["ms"]) / 1000
insns = int(m["took_ms"]) * 1_000_000            # 1 ns of virtual time = 1 instruction
per_s = insns / audio_s
print(f"instructions: {insns/1e6:.0f} M for {audio_s:.0f} s of audio = {per_s/1e6:.1f} M/s, "
      f"{per_s/44100:.0f} per stereo frame")
for mhz in (240, 160):
    lo, hi = per_s / (mhz * 1e6), 1.5 * per_s / (mhz * 1e6)
    print(f"  one core at {mhz} MHz: {100*lo:.0f} % (CPI 1.0) .. {100*hi:.0f} % (CPI 1.5)")
PY
