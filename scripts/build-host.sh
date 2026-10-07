#!/usr/bin/env bash
# Build the native Linux target: the SLOOP core (same sources as the firmware), the host app and
# the port's host tests, into build-host/.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
CC="$(host_cc)"
export CC
SLOOP_PYTHON="$(find_pillow_python)"
export SLOOP_PYTHON
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
if [[ -f "$ROOT_DIR/build-host/CMakeCache.txt" ]] && ! grep -q "CMAKE_C_COMPILER:.*=$(command -v "$CC")" "$ROOT_DIR/build-host/CMakeCache.txt" 2>/dev/null; then
  echo "build-host was configured with another compiler; reconfiguring"
  rm -rf "$ROOT_DIR/build-host"
fi
cmake -S "$ROOT_DIR/host" -B "$ROOT_DIR/build-host" "${GEN[@]}" >/dev/null
cmake --build "$ROOT_DIR/build-host" -j"$(nproc)"
echo "host build: OK ($ROOT_DIR/build-host)"
