#!/usr/bin/env bash
# Host test ladder: project layout checks, upstream SLOOP's own tests, the port's host tests.
#   SKIP_UPSTREAM=1  skip upstream's suite (faster iterations on the port)
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
fail=0
echo "### project layout (pytest)"
PY="$(command -v python3)"
if "$PY" -c 'import pytest' >/dev/null 2>&1; then
  "$PY" -m pytest -q "$ROOT_DIR/tests" || fail=1
else
  echo "pytest missing (python3 -m pip install --user pytest); skipping layout checks"
fi
if [[ "${SKIP_UPSTREAM:-0}" != 1 ]]; then
  echo "### upstream SLOOP host tests"
  "$ROOT_DIR/scripts/test-upstream.sh" || fail=1
fi
echo "### port host tests"
"$ROOT_DIR/scripts/build-host.sh"
ctest --test-dir "$ROOT_DIR/build-host" --output-on-failure || fail=1
[[ $fail -eq 0 ]] && echo "ALL HOST TESTS PASSED" || { echo "HOST TESTS FAILED"; exit 1; }
