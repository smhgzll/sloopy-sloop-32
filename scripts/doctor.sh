#!/usr/bin/env bash
# Check the development environment and say exactly what is missing.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
set +e

fail=0 warn=0
ok()   { printf '[OK]   %s\n' "$1"; }
miss() { printf '[MISS] %s\n       -> %s\n' "$1" "$2"; fail=1; }
note() { printf '[WARN] %s\n       -> %s\n' "$1" "$2"; warn=1; }
have() { command -v "$1" >/dev/null 2>&1; }

echo "== host tools"
for t in git cmake python3; do have "$t" && ok "$t" || miss "$t" "sudo dnf install $t"; done
have ninja && ok "ninja" || note "ninja (faster host builds)" "sudo dnf install ninja-build"
if cc="$(host_cc 2>/dev/null)"; then ok "C compiler: $cc ($($cc --version | head -1))"; else miss "C compiler" "sudo dnf install gcc"; fi
if [[ -n "${CC:-}" ]] && ! have "$CC"; then note "CC=$CC is set but not installed" "unset CC (scripts fall back to cc/gcc)"; fi
py="$(find_pillow_python 2>/dev/null)" && ok "Python with Pillow: $py" || miss "Python 3 with Pillow (upstream asset generators)" "sudo dnf install python3-pillow"
python3 -c 'import pytest' 2>/dev/null && ok "pytest" || note "pytest (layout checks)" "python3 -m pip install --user pytest"
have node && ok "node $(node --version) (web tests)" || note "node (web panel tests)" "sudo dnf install nodejs"
pkg-config --exists alsa 2>/dev/null && ok "ALSA headers (host audio)" || note "alsa-lib-devel (sloop_host audio output)" "sudo dnf install alsa-lib-devel"
for c in google-chrome chromium chromium-browser; do have "$c" && { ok "headless browser: $c (screenshots)"; break; }; done

echo "== upstream SLOOP"
UP="$ROOT_DIR/upstream/sloop-fm1"
if [[ -d "$UP/.git" ]]; then
  head="$(git -C "$UP" rev-parse HEAD)"; lock="$(sed -n 's/^SLOOP_COMMIT=//p' "$ROOT_DIR/config/upstream.lock" 2>/dev/null)"
  [[ "$head" == "$lock" ]] && ok "upstream at the locked commit ${head:0:12}" || note "upstream at ${head:0:12}, lock says ${lock:0:12}" "scripts/fetch-sloop.sh"
else
  miss "upstream SLOOP checkout" "scripts/fetch-sloop.sh"
fi

echo "== ESP-IDF"
if [[ -f "$IDF_PATH/export.sh" ]]; then
  ok "ESP-IDF at $IDF_PATH ($(git -C "$IDF_PATH" describe --tags 2>/dev/null))"
  if source "$IDF_PATH/export.sh" >/dev/null 2>&1; then
    have idf.py && ok "idf.py" || miss "idf.py" "scripts/bootstrap-nobara.sh"
    have xtensa-esp32s3-elf-gcc && ok "xtensa-esp32s3-elf-gcc $(xtensa-esp32s3-elf-gcc -dumpversion)" || miss "ESP32-S3 toolchain" "$IDF_PATH/install.sh esp32s3"
    have qemu-system-xtensa && ok "qemu-system-xtensa ($(qemu-system-xtensa --version | head -1 | sed 's/.*(//; s/)//'))" || miss "Espressif QEMU" "python \$IDF_PATH/tools/idf_tools.py install qemu-xtensa"
    if have qemu-system-xtensa; then
      qemu-system-xtensa -machine help 2>/dev/null | grep -q esp32s3 && ok "QEMU machine esp32s3" || miss "QEMU esp32s3 machine" "update qemu-xtensa"
    fi
  else
    miss "ESP-IDF environment (export.sh failed)" "$IDF_PATH/install.sh esp32s3"
  fi
else
  miss "ESP-IDF checkout" "scripts/bootstrap-nobara.sh"
fi
libs="$(ldconfig -p 2>/dev/null)"
for lib in libSDL2-2.0.so.0 libslirp.so.0; do
  [[ "$libs" == *"$lib"* ]] && ok "$lib (QEMU)" || note "$lib (QEMU needs it)" "sudo dnf install SDL2 libslirp (SDL2 = sdl2-compat on Fedora 42+)"
done

echo "== project"
[[ -f "$ROOT_DIR/config/local.env" ]] && ok "config/local.env (Wi-Fi, I2S pins)" || note "no config/local.env (hardware builds use defaults: no I2S pins)" "cp config/local.env.example config/local.env && \$EDITOR config/local.env"
[[ -f "$ROOT_DIR/assets/references/mwave-fm1-front-reference.svg" ]] && ok "FM-1 reference assets" || note "FM-1 reference SVG missing" "git checkout assets/references"
for d in /dev/ttyACM* /dev/ttyUSB*; do [[ -e "$d" ]] && { ok "serial device $d"; [[ -w "$d" ]] || note "$d not writable" "sudo usermod -aG dialout \$USER (log out and in)"; }; done

echo
if (( fail )); then echo "Some requirements are missing (see above)."; exit 1; fi
(( warn )) && echo "Environment usable, with warnings." || echo "Environment looks ready."
exit 0
