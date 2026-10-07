#!/usr/bin/env bash
# Set up a Nobara / Fedora machine for sloopy-sloop-32: system packages (with sudo, shown first),
# ESP-IDF (the version in config/upstream.lock) with the ESP32-S3 toolchain and QEMU, upstream SLOOP.
# Idempotent: re-running only completes what is missing. SKIP_DNF=1 skips the system packages.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LOCK="$ROOT_DIR/config/upstream.lock"
ESP_IDF_REPO="${ESP_IDF_REPO:-https://github.com/espressif/esp-idf.git}"
ESP_IDF_REF="${ESP_IDF_REF:-$(sed -n 's/^ESP_IDF_REF=//p' "$LOCK" 2>/dev/null)}"
ESP_IDF_REF="${ESP_IDF_REF:-v5.5.5}"
IDF_PATH="${IDF_PATH:-$ROOT_DIR/tools/esp-idf}"

command -v dnf >/dev/null 2>&1 || { echo "This bootstrap is for Nobara / Fedora (dnf)." >&2; exit 1; }

PKGS=(git wget curl flex bison gperf python3 python3-pip python3-pillow python3-pytest cmake ninja-build
      ccache dfu-util libusb1-devel gcc gcc-c++ make patch unzip tar xz SDL2 libslirp libgcrypt glib2
      pixman alsa-lib-devel nodejs)
echo "[1/4] System packages"
missing=()
for p in "${PKGS[@]}"; do rpm -q --whatprovides "$p" >/dev/null 2>&1 || missing+=("$p"); done
if (( ${#missing[@]} )) && [[ "${SKIP_DNF:-0}" != 1 ]]; then
  echo "  missing: ${missing[*]}"
  echo "  running: sudo dnf install -y ${missing[*]}"
  sudo dnf install -y "${missing[@]}"
else
  echo "  nothing to install"
fi

echo "[2/4] ESP-IDF $ESP_IDF_REF in $IDF_PATH"
if [[ ! -d "$IDF_PATH/.git" ]]; then
  git clone --depth 1 --branch "$ESP_IDF_REF" --recursive --shallow-submodules -j8 "$ESP_IDF_REPO" "$IDF_PATH"
else
  echo "  present ($(git -C "$IDF_PATH" describe --tags 2>/dev/null))"
fi

echo "[3/4] ESP32-S3 toolchain and QEMU (~/.espressif)"
"$IDF_PATH/install.sh" esp32s3
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa

echo "[4/4] Upstream SLOOP"
"$ROOT_DIR/scripts/fetch-sloop.sh"

echo
echo "Bootstrap complete. Check with: scripts/doctor.sh"
