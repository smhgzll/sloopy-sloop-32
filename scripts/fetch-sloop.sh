#!/usr/bin/env bash
# Fetch upstream SLOOP into upstream/sloop-fm1 (gitignored: not vendored).
# Checks out the known-good commit from config/upstream.lock; SLOOP_REF=<ref> overrides
# (e.g. SLOOP_REF=main to try the latest, then update the lock once the tests pass).
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT_DIR/upstream/sloop-fm1"
LOCK="$ROOT_DIR/config/upstream.lock"
REPO="${SLOOP_REPO:-https://github.com/isod89/sloop-fm1.git}"
REF="${SLOOP_REF:-}"
if [[ -z "$REF" && -f "$LOCK" ]]; then
  REF="$(sed -n 's/^SLOOP_COMMIT=//p' "$LOCK")"
fi
REF="${REF:-main}"

if [[ ! -d "$DEST/.git" ]]; then
  git clone "$REPO" "$DEST"
fi
git -C "$DEST" fetch --all --tags --quiet
git -C "$DEST" checkout --quiet "$REF"
if [[ "$REF" == "main" || "$REF" == "master" ]]; then
  git -C "$DEST" pull --ff-only --quiet || true
fi

echo "SLOOP path:   $DEST"
echo "SLOOP commit: $(git -C "$DEST" rev-parse HEAD)"
if [[ -f "$LOCK" ]] && [[ "$(git -C "$DEST" rev-parse HEAD)" != "$(sed -n 's/^SLOOP_COMMIT=//p' "$LOCK")" ]]; then
  echo "note: this is not the commit in config/upstream.lock; run the tests before updating the lock"
fi
if [[ -f "$DEST/LICENSE" ]]; then
  cp "$DEST/LICENSE" "$ROOT_DIR/upstream/SLOOP-LICENSE.txt"
fi
