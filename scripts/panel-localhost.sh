#!/usr/bin/env bash
# The board's panel at http://localhost:8080/ (Ctrl-C stops): scripts/panel-localhost.sh [device] [port]
# Browsers give Web MIDI (the panel's "midi in": a MIDI keyboard on this computer plays SLOOP) only
# to https:// or localhost pages; the board serves plain http. This forwards a local port to it
# (HTTP and the WebSocket). device: an IP or name (default sloopy.local, SLOOPY_HOSTNAME).
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
load_local_env
DEVICE="${1:-${SLOOPY_HOSTNAME:-sloopy}.local}"
PORT="${2:-8080}"
echo "panel: http://localhost:$PORT/  ->  http://$DEVICE/   (Ctrl-C stops)"
if command -v socat >/dev/null; then
  exec socat "TCP-LISTEN:$PORT,bind=127.0.0.1,fork,reuseaddr" "TCP:$DEVICE:80"
fi
exec python3 - "$DEVICE" "$PORT" <<'PY'
import asyncio, sys
dev, port = sys.argv[1], int(sys.argv[2])
async def pipe(r, w):
    try:
        while (d := await r.read(65536)):
            w.write(d); await w.drain()
    finally:
        w.close()
async def client(r, w):
    try:
        dr, dw = await asyncio.open_connection(dev, 80)
    except OSError as e:
        print(f"{dev}: {e}", file=sys.stderr); w.close(); return
    await asyncio.gather(pipe(r, dw), pipe(dr, w), return_exceptions=True)
async def main():
    async with await asyncio.start_server(client, "127.0.0.1", port) as s:
        await s.serve_forever()
asyncio.run(main())
PY
