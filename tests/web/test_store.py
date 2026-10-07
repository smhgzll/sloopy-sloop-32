#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""SLOOP's store as a backup file (GET / POST /api/store) against the firmware (QEMU or a board):

    test_store.py --connect 127.0.0.1:18080 [--log qemu.log]

A tempo A is set with SLOOP's editor (SysEx over the WebSocket) and autosaved; a backup is
downloaded (SLOOP's whole store window, its objects valid); a tempo B is set and autosaved; a wrong
password, a wrong size and other data are refused; the backup is restored: the device restarts,
the boot copies it in before SLOOP starts, and tempo A is back. (A backup holds what SLOOP saved:
SLOOP autosaves only when nothing sounds, 2.5 s after the last touch, 20 s after its last save.)
This reboots the target."""
import argparse
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wsclient import WsClient  # noqa: E402

STORE_LO, STORE_BYTES = 0x90000, 0x70000
fails = 0
T0 = time.time()


def check(ok, what):
    global fails
    print(f"  {what:<70} {'ok' if ok else 'FAIL'}  [{time.time() - T0:5.1f} s]")
    if not ok:
        fails += 1


def status(base, timeout=10):
    return json.loads(urllib.request.urlopen(f"{base}/api/status", timeout=timeout).read())


def post(base, body, password, timeout=120):
    req = urllib.request.Request(f"{base}/api/store", data=body, method="POST",
                                 headers={"Content-Type": "application/octet-stream",
                                          "X-Sloopy-Password": urllib.parse.quote(password, safe="")})
    try:
        r = urllib.request.urlopen(req, timeout=timeout)
        return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def valid_objects(img):
    """storage.c's commit records (magic "FELU", header CRC, payload CRC) in the image"""
    n = 0
    for off in range(0, STORE_BYTES, 4096):
        magic, typ, slot, seq, length, crc = struct.unpack_from("<IHHIII", img, off)
        if magic != 0x554C4546 or length > 4096 - 256:
            continue
        hcrc = struct.unpack_from("<I", img, off + 28)[0]
        if zlib.crc32(img[off:off + 28]) == hcrc and zlib.crc32(img[off + 256:off + 256 + length]) == crc:
            n += 1
    return n


class Editor:
    """SLOOP's web editor protocol (upstream web/EDITOR_PROTOCOL.md) over the WebSocket"""
    def __init__(self, host, port):
        self.ws = WsClient(host, port)
        self.ws.send_json({"v": 1, "t": "hello", "client": "store-test", "screen": False})
        self.ws.wait_for(lambda k, m: k == "text" and m.get("t") == "hello", 5)

    def req(self, cmd, *args):
        self.ws.send_json({"v": 1, "t": "sysex", "data": bytes([0xF0, 0x7D, 0x46, 0x4C, cmd, *args, 0xF7]).hex()})
        kind, m = self.ws.wait_for(lambda k, m: k == "text" and m.get("t") == "sysex"
                                   and bytes.fromhex(m["data"])[4] == cmd, 3)
        return bytes.fromhex(m["data"])

    def set_bpm(self, bpm):
        v = bpm + 8192
        r = self.req(3, 1, 0, v & 0x7F, v >> 7)              # SET global G_BPM
        return (r[7] | r[8] << 7) - 8192


def set_and_save(base, host, port, bpm):
    """the tempo through the editor, then SLOOP's autosave (up to its 20 s gap + 2.5 s idle)"""
    w0 = status(base)["store"]["writes"]
    ed = Editor(host, int(port))
    got = ed.set_bpm(bpm)
    ed.ws.close()
    end = time.time() + 45
    while time.time() < end and status(base)["store"]["writes"] == w0:
        time.sleep(1)
    st = status(base)
    return got == bpm and st["bpm"] == bpm and st["store"]["writes"] > w0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--connect", required=True)
    ap.add_argument("--password", default="change-this-password")   # the build's placeholder
    ap.add_argument("--log")
    a = ap.parse_args()
    host, port = a.connect.rsplit(":", 1)
    base = f"http://{host}:{port}"
    print("store backup (GET / POST /api/store):")

    st = status(base)
    check(st.get("store", {}).get("backup") == 1, "status: backups available")
    tempo_a = st["bpm"] + 7 if st["bpm"] < 200 else st["bpm"] - 7
    tempo_b = tempo_a + 11
    check(set_and_save(base, host, port, tempo_a), f"the editor sets tempo A = {tempo_a}, the autosave writes it")
    r = urllib.request.urlopen(f"{base}/api/store", timeout=60)
    backup = r.read()
    nobj = valid_objects(backup)
    check(len(backup) == STORE_BYTES and "sloop-store-" in r.headers.get("Content-Disposition", ""),
          f"backup: SLOOP's store window, {len(backup) // 1024} KiB, as a file")
    check(nobj >= 1, f"backup: SLOOP's saved objects are valid in it ({nobj} sector copies)")
    check(set_and_save(base, host, port, tempo_b), f"then tempo B = {tempo_b}, autosaved too")
    want = tempo_b

    code, j = post(base, backup, "wrong-password")
    check(code == 403, "restore refused: the wrong Wi-Fi password")
    code, j = post(base, backup[:100000], a.password)
    check(code == 400 and "exactly" in j.get("error", ""), f"restore refused: the wrong size ({j.get('error')})")
    other = bytes((i * 2654435761 >> 13) & 0xFF for i in range(STORE_BYTES))
    code, j = post(base, other, a.password)
    check(code == 400 and "not a SLOOP store" in j.get("error", ""), "restore refused: other data")
    check(status(base)["bpm"] == want, "after the refusals: nothing changed")

    boot = status(base)["boot"]
    code, j = post(base, backup, a.password)
    check(code == 200 and j.get("restart") and j.get("autosave") == 1,
          f"restore: accepted ({j.get('projects')} projects, autosave {j.get('autosave')}), restarting")
    after, end = None, time.time() + 120
    time.sleep(3)
    while time.time() < end:
        try:
            s = status(base, timeout=5)
            if s["boot"] != boot:
                after = s
                break
        except (urllib.error.URLError, ConnectionError, OSError, ValueError, KeyError):
            pass
        time.sleep(1)
    check(after is not None and after["bpm"] == tempo_a,
          f"restored: tempo A ({tempo_a}) is back ({(after or {}).get('bpm')})")
    if a.log:
        log = open(a.log, errors="replace").read()
        check("store: restored from a backup" in log, "console: the boot copied the backup in before SLOOP")
    print(f"store backup: {'PASS' if not fails else 'FAIL'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
