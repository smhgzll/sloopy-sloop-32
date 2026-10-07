#!/usr/bin/env bash
# Run upstream SLOOP's own host tests (upstream/sloop-fm1/tests) without the JieLi toolchain.
# Mirrors upstream tests/run_tests.sh; skips only the entries that need the JieLi-built package
# (build/felucca.fwsc, build/felucca.dis): ota_test, ldr_test, target_budget.py.
#   SOAK_MIN=n  soak test minutes (default 1; upstream default 10)
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
UP="$ROOT_DIR/upstream/sloop-fm1"
[[ -f "$UP/firmware/src/felucca.c" ]] || { echo "upstream SLOOP missing: run scripts/fetch-sloop.sh" >&2; exit 1; }
PY="$(find_pillow_python)" || exit 1
HOSTCC="$(host_cc)" || exit 1
cd "$UP"
OUT=build/host
mkdir -p "$OUT" build/gen build/tracks_demo build/slicer_demo
echo "== generating upstream headers ($PY)"
for g in font:felucca_font icons:felucca_icons tables:felucca_tables samples:felucca_samples \
         drumkits:felucca_drumkits logo:sloop_logo; do
  "$PY" "tools/gen_${g%%:*}.py" "build/gen/${g##*:}.h" >/dev/null || { echo "gen_${g%%:*} failed" >&2; exit 1; }
done
CC="$HOSTCC -O1 -Wall -Wno-unused-function"
fail=0; skipped=0; passed=0
run() {
  local name="$1"; shift
  printf '== %-70s ' "$name"
  if "$@" >"$OUT/last.log" 2>&1; then echo PASS; passed=$((passed+1)); else echo FAIL; tail -20 "$OUT/last.log"; fail=1; fi
}
build() { "$@" 2>"$OUT/build.log" || { echo "build failed: $*"; cat "$OUT/build.log"; fail=1; return 1; }; }
build $CC -o "$OUT/storage_test" tests/storage_test.c && run "flash storage (A/B, torn writes)" "$OUT/storage_test"
build $CC -o "$OUT/recovery_test" tests/recovery_test.c && run "USB recovery and boot-loop guard" "$OUT/recovery_test"
build $CC -o "$OUT/arranger_test" tests/arranger_test.c && run "song order, timing, repeats" "$OUT/arranger_test"
G="$HOSTCC -O2 -w -Ibuild/gen -Ifirmware/src"
build $G -o "$OUT/song_audio_test" tests/song_audio_test.c -lm && run "song: four tracks, scene transition, stop" "$OUT/song_audio_test" "$OUT/song-demo.wav"
build $G -o "$OUT/song_ui_test" tests/song_ui_test.c -lm && run "song screen" "$OUT/song_ui_test" "$OUT/song-screen.ppm"
build $G -o "$OUT/studio_drums_test" tests/studio_drums_test.c -lm && run "drum lanes, kits, metronome, record, free take" "$OUT/studio_drums_test" "$OUT/drum-styles.wav"
build $G -o "$OUT/seq2_test" tests/seq2_test.c -lm && run "sequencer 2.0" "$OUT/seq2_test"
build $G -o "$OUT/drumkit_test" tests/drumkit_test.c -lm && run "synthesised drum kits" "$OUT/drumkit_test" "$OUT/drum-kits.wav" "$OUT/drum-kits.txt"
build $G -o "$OUT/punch_test" tests/punch_test.c -lm && run "punch-in FX" "$OUT/punch_test" "$OUT/punch-fx.wav"
build $G -Ifirmware/hal -o "$OUT/ui_pages_test" tests/ui_pages_test.c -lm && run "live UI: pages, layers, holds, fuzz" "$OUT/ui_pages_test" "$OUT"
build $G -o "$OUT/soak_test" tests/soak_test.c -lm && run "soak: ${SOAK_MIN:-1} min random live use" "$OUT/soak_test" "${SOAK_MIN:-1}"
build $CC -o "$OUT/upreset_test" tests/upreset_test.c && run "user presets" "$OUT/upreset_test"
build $CC -o "$OUT/midi_uart_test" tests/midi_uart_test.c && run "TRS MIDI parser" "$OUT/midi_uart_test"
build $G -o "$OUT/hostsim" tests/hostsim.c -lm
build $G -o "$OUT/scale_test" tests/scale_test.c -lm && run "scales" "$OUT/scale_test"
run "DSP render (ANALOG preset 0)" "$OUT/hostsim" 0 0 1 "$OUT/render.wav"
run "TRACKS: 4-track pattern, recording, voice budget" env TRACKS=build/tracks_demo "$OUT/hostsim" 0 0 1 "$OUT/tracks.wav"
build $CC -w -Ibuild/gen -Ifirmware/src -o "$OUT/project_test" tests/project_test.c -lm && run "project formats, autosave" "$OUT/project_test"
build $G -o "$OUT/slicer_test" tests/slicer_test.c -lm && run "SLICER" "$OUT/slicer_test" build/slicer_demo
build $G -o "$OUT/regress" tests/regress.c -lm && run "regression: 97 golden renders, health, voices, CPU" "$OUT/regress" tests/golden.txt tests/cpu_baseline.txt
run "installer CLI against a simulated FM-1" "$PY" tests/install_test.py
if command -v node >/dev/null 2>&1; then
  run "web pages: editor protocol, samples, packages" node web/test_web.mjs
else
  echo "== skip web tests (no node)"; skipped=$((skipped+1))
fi
echo "== skip ota_test, ldr_test, target_budget.py (need the JieLi-built felucca.fwsc / felucca.dis)"
skipped=$((skipped+3))
echo "upstream: $passed passed, $skipped skipped, upstream commit $(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
[[ $fail -eq 0 ]] && echo "UPSTREAM HOST TESTS PASSED" || { echo "UPSTREAM HOST TESTS FAILED"; exit 1; }
