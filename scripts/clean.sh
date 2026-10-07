#!/usr/bin/env bash
# Remove build outputs (not the toolchain, not upstream, not config/local.env).
#   scripts/clean.sh            build-host, build-qemu, build-hw
#   scripts/clean.sh --upstream also upstream's generated build/ (headers, test outputs)
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
rm -rf "$ROOT_DIR/build-host" "$ROOT_DIR/build-qemu" "$ROOT_DIR/build-hw" "$ROOT_DIR"/qemu-*.log
[[ "${1:-}" == --upstream ]] && rm -rf "$ROOT_DIR/upstream/sloop-fm1/build"
echo "Build outputs removed."
