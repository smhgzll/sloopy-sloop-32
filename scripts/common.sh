#!/usr/bin/env bash
# Shared helpers for the project scripts (sourced).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export ROOT_DIR
export IDF_PATH="${IDF_PATH:-$ROOT_DIR/tools/esp-idf}"

source_idf() {
  if [[ ! -f "$IDF_PATH/export.sh" ]]; then
    echo "ESP-IDF not found at $IDF_PATH" >&2
    echo "Run: $ROOT_DIR/scripts/bootstrap-nobara.sh" >&2
    exit 1
  fi
  # shellcheck disable=SC1091
  source "$IDF_PATH/export.sh" >/dev/null
}

load_local_env() {
  if [[ -f "$ROOT_DIR/config/local.env" ]]; then
    set -a
    # shellcheck disable=SC1091
    source "$ROOT_DIR/config/local.env"
    set +a
  fi
}

# A Python 3 with Pillow (upstream's asset generators need it); SLOOP_PYTHON overrides.
find_pillow_python() {
  local py
  for py in "${SLOOP_PYTHON:-}" "$(command -v python3 2>/dev/null || true)" /usr/bin/python3 /usr/local/bin/python3; do
    [[ -n "$py" && -x "$py" ]] || continue
    if "$py" -c 'import PIL' >/dev/null 2>&1; then echo "$py"; return 0; fi
  done
  echo "No Python 3 with Pillow found. Install: sudo dnf install python3-pillow (or set SLOOP_PYTHON)" >&2
  return 1
}

# A working host C compiler: $CC if it exists, else cc / gcc / clang. (Some shells export a CC
# that is not installed, e.g. CC=gcc-14 on a GCC 16 system.)
host_cc() {
  local c
  for c in "${SLOOPY_HOST_CC:-}" "${CC:-}" cc gcc clang; do
    [[ -n "$c" ]] || continue
    if command -v "$c" >/dev/null 2>&1; then echo "$c"; return 0; fi
    if [[ "$c" == "${CC:-}" ]]; then echo "note: CC=$c is not installed; trying another compiler" >&2; fi
  done
  echo "No host C compiler found. Install: sudo dnf install gcc" >&2
  return 1
}

# ESP-IDF applies sdkconfig.defaults only when it creates an sdkconfig. The build dirs' sdkconfig is
# derived from the tracked defaults (+ config/sdkconfig.local.defaults): regenerate it when their
# content changed (a hash of them is kept next to it). Persistent settings belong in those files.
refresh_sdkconfig() {
  local build_dir="$1"; shift
  local cfg="$build_dir/sdkconfig" stamp="$build_dir/.sloopy-defaults.sha256" sum
  sum="$(cat "$@" "$ROOT_DIR/main/Kconfig.projbuild" 2>/dev/null | sha256sum | cut -d' ' -f1)"
  if [[ -f "$cfg" ]] && [[ "$(cat "$stamp" 2>/dev/null)" != "$sum" ]]; then
    echo "note: the sdkconfig defaults changed: regenerating $cfg"
    rm -f "$cfg"
  fi
  mkdir -p "$build_dir"
  echo "$sum" > "$stamp"
}
