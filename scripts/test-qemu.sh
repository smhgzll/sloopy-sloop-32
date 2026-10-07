#!/usr/bin/env bash
# Build the QEMU profile, boot it in ESP32-S3 QEMU and run the firmware self-test (main/selftest.c):
#   phase 1  cross-target render hash (must equal the host's), boot, screens, transport, keys,
#            encoders, PSRAM, autosave into the "sloop" partition, then esp_restart
#   phase 2  the project is restored from flash after the restart
#   web      then the browser panel's end-to-end WebSocket test (tests/web/test_ws_e2e.py) against
#            the firmware's own web server, over QEMU's emulated Ethernet (port QEMU_WEB_PORT)
#   store    a second boot of the same flash (the self-test, passed, stays idle) with single-threaded
#            TCG (KNOWN_ISSUES KI-9): a backup of SLOOP's store, a change, the backup restored
#            (tests/web/test_store.py; the device restarts and copies it in before SLOOP starts), then
#            the same with the device page's link and form in headless Chrome
#   ota      then a firmware update through POST /api/ota (tests/web/test_ota.py):
#            refusals, then the build's own image into the other slot, the restart, the new image
#            confirming itself; then the same from the device page in headless Chrome (if installed)
# Pass/fail from the console markers. QEMU does not emulate I2S or Wi-Fi: not tested here.
#   QEMU_TIMEOUT=s (default 300)
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$ROOT_DIR/build-qemu/qemu-test.log"
TIMEOUT="${QEMU_TIMEOUT:-300}"

"$ROOT_DIR/scripts/build-qemu.sh" >/dev/null || { echo "QEMU test: FAIL (firmware build)"; exit 1; }

# the host's hash of the same deterministic render
EXPECT=""
if "$ROOT_DIR/scripts/build-host.sh" >/dev/null 2>&1; then
  EXPECT="$("$ROOT_DIR/build-host/test_port_selftest" "$ROOT_DIR/build-host/out" | sed -n 's/^SLOOP_SELFTEST_RENDER .*hash=\([0-9a-f]*\).*/\1/p')"
fi

rm -f "$LOG"
QEMU_LOG="$LOG" "$ROOT_DIR/scripts/run-qemu.sh" &
QPID=$!
result=""
WEB_PORT="${QEMU_WEB_PORT:-18080}"
export QEMU_WEB_PORT="$WEB_PORT"
web=""
ota=""
store=""
for ((t = 0; t < TIMEOUT; t++)); do
  sleep 1
  if grep -q "SLOOP_QEMU_PASS" "$LOG" 2>/dev/null; then
    result=PASS
    echo "---- web panel end to end, against the firmware in QEMU (127.0.0.1:$WEB_PORT)"
    if python3 "$ROOT_DIR/tests/web/test_ws_e2e.py" --connect "127.0.0.1:$WEB_PORT"; then web=PASS; else web=FAIL; result=FAIL; fi
    break
  fi
  if grep -qE "SLOOP_QEMU_FAIL|Guru Meditation|abort\(\) was called|Backtrace:" "$LOG" 2>/dev/null; then
    sleep 2; result=FAIL; break
  fi
  kill -0 "$QPID" 2>/dev/null || { result=FAIL; break; }
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
[[ -z "$result" ]] && result=TIMEOUT

# the firmware update, on the flash the self-test left (KEEP_FLASH): single-threaded TCG, as this
# QEMU's multi-threaded TCG can abort while one core remaps flash pages the other executes (KI-9)
if [[ "$result" == PASS ]]; then
  OTA_LOG="$ROOT_DIR/build-qemu/qemu-ota.log"
  rm -f "$OTA_LOG"
  echo "---- backup / restore and firmware update from the browser, against the firmware in QEMU"
  KEEP_FLASH=1 QEMU_TCG_THREAD=single QEMU_LOG="$OTA_LOG" "$ROOT_DIR/scripts/run-qemu.sh" &
  QPID=$!
  for ((t = 0; t < 60; t++)); do
    sleep 1
    grep -q "SLOOP_QEMU_IDLE" "$OTA_LOG" 2>/dev/null && grep -q "net: IP" "$OTA_LOG" 2>/dev/null && break
  done
  if python3 "$ROOT_DIR/tests/web/test_store.py" --connect "127.0.0.1:$WEB_PORT" --log "$OTA_LOG"; then
    store=PASS; else store=FAIL; result=FAIL; fi
  # and as a user does it: the device page's link and form in headless Chrome
  if [[ "$store" == PASS ]] && ! python3 "$ROOT_DIR/tests/web/test_browser.py" --connect "127.0.0.1:$WEB_PORT" \
       --backup; then store=FAIL; result=FAIL; fi
  if python3 "$ROOT_DIR/tests/web/test_ota.py" --connect "127.0.0.1:$WEB_PORT" \
       --image "$ROOT_DIR/build-qemu/sloopy_sloop_32.bin" --log "$OTA_LOG"; then ota=PASS; else ota=FAIL; result=FAIL; fi
  # and as a user does it: the device page in headless Chrome (back into the first slot)
  if [[ "$ota" == PASS ]] && ! python3 "$ROOT_DIR/tests/web/test_browser.py" --connect "127.0.0.1:$WEB_PORT" \
       --ota-image "$ROOT_DIR/build-qemu/sloopy_sloop_32.bin"; then ota=FAIL; result=FAIL; fi
  kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
fi

GOT="$(sed -n 's/^SLOOP_SELFTEST_RENDER .*hash=\([0-9a-f]*\).*/\1/p' "$LOG" | head -1)"
echo "---- selftest (log: $LOG)"
grep -E "selftest:|SLOOP_|sloop_plat: store|sloopy: (flash|heap|upstream)|net: IP|web: " "$LOG" | sed 's/\x1b\[[0-9;]*m//g'
echo "----"
if [[ -n "$EXPECT" ]]; then
  if [[ "$GOT" == "$EXPECT" ]]; then
    echo "cross-target render: ESP32-S3 (QEMU) $GOT == host $EXPECT"
  else
    echo "cross-target render: MISMATCH ESP32-S3 (QEMU) '$GOT' != host '$EXPECT'"
    result=FAIL
  fi
else
  echo "cross-target render: host hash unavailable (host build failed); not compared"
fi
[[ -n "$web" ]] && echo "web panel over QEMU Ethernet: $web"
[[ -n "$store" ]] && echo "projects backup / restore over the web: $store"
[[ -n "$ota" ]] && echo "firmware update over the web (OTA slot switch, rollback armed): $ota"
if [[ "$result" == PASS ]]; then
  echo "QEMU test: PASS"
  exit 0
fi
echo "QEMU test: $result"
tail -40 "$LOG" | sed 's/\x1b\[[0-9;]*m//g'
exit 1
