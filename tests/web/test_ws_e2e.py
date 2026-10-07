#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""End to end: the panel protocol over a real WebSocket against a running SLOOP.

    test_ws_e2e.py --host-app build-host/sloop_host --web web     (starts the host app)
    test_ws_e2e.py --connect 127.0.0.1:8080                       (an already running target:
                                                                   the host app or the ESP32)
Checks: the panel page is served, hello + whole screen + LEDs + status on connect, a PLAY tap
starts and stops the transport, the screen updates arrive as rectangles, keys and encoders are
accepted, ping/pong round trip, malformed messages get an error and the session survives."""
import argparse
import gzip
import json
import os
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wsclient import WsClient, parse_rect  # noqa: E402

fails = 0


def fetch(url, timeout=20, tries=3):
    """GET url (a retry on a dropped connection: emulated networks lose packets)"""
    for i in range(tries):
        try:
            resp = urllib.request.urlopen(url, timeout=timeout)
            body = resp.read()
            if resp.headers.get("Content-Encoding") == "gzip":
                body = gzip.decompress(body)
            return body
        except (urllib.error.URLError, ConnectionError, OSError):
            if i == tries - 1:
                raise
            time.sleep(0.5)


def check(ok, what):
    global fails
    print(f"  {what:<70} {'ok' if ok else 'FAIL'}")
    if not ok:
        fails += 1


def is_text(t):
    return lambda kind, msg: kind == "text" and msg.get("t") == t


class Screen:
    """the screen as a client rebuilds it: the whole screen, then every rectangle on top"""
    def __init__(self):
        self.fb = bytearray(240 * 240 * 2)
        self.rects = 0

    def apply(self, b):
        x, y, w, h, px = parse_rect(b)
        for row in range(h):
            o = ((y + row) * 240 + x) * 2
            self.fb[o:o + w * 2] = px[row * w * 2:(row + 1) * w * 2]
        self.rects += 1


def editor(host, port):
    """SLOOP's web editor protocol (SysEx, upstream web/EDITOR_PROTOCOL.md) over the WebSocket"""
    for path, want in (("/editor.html", "editor-bridge.js"), ("/editor-bridge.js", "requestMIDIAccess")):
        body = fetch(f"http://{host}:{port}{path}")
        check(want in body.decode(errors="replace"), f"editor: {path} is served")
    ed = WsClient(host, port)
    ed.send_json({"v": 1, "t": "hello", "client": "editor", "screen": False})
    ed.wait_for(is_text("hello"), 5)

    def req(cmd, *args, timeout=3):
        ed.send_json({"v": 1, "t": "sysex", "data": bytes([0xF0, 0x7D, 0x46, 0x4C, cmd, *args, 0xF7]).hex()})
        kind, m = ed.wait_for(lambda k, m: k == "text" and m.get("t") == "sysex"
                              and bytes.fromhex(m["data"])[4] == cmd, timeout)
        return bytes.fromhex(m["data"])

    t0 = time.time()
    info = req(1)
    names = [x.decode() for x in info[5:-1].split(b"\0")]
    check(info[:5] == bytes([0xF0, 0x7D, 0x46, 0x4C, 1]) and info[-1] == 0xF7 and "SLOOP" in names[0],
          f"editor: INFO answers ({names[0]}, {(time.time() - t0) * 1000:.0f} ms)")
    check(info[-2] == 5, "editor: protocol version 5")
    v = 133 + 8192
    r = req(3, 1, 0, v & 0x7F, v >> 7)                   # SET global G_BPM = 133
    got = (r[7] | r[8] << 7) - 8192
    r = req(2, 1, 0)                                      # GET it back
    back = (r[7] | r[8] << 7) - 8192
    check(got == 133 and back == 133, "editor: SET / GET the tempo (133 BPM)")
    smp = req(15)
    check(smp[5] == 3 and smp[6] == 80, "editor: SMP_INFO: 3 user sample slots of 80 KiB")
    check(req(25)[5] == 0, "editor: PING")
    kinds = set()
    end = time.time() + 0.8
    while time.time() < end:
        try:
            ed.s.settimeout(0.2)
            kinds.add(ed.recv()[0])
        except Exception:  # noqa: BLE001
            pass
    check("bin" not in kinds, "editor: an editor client gets no screen frames")
    v = 120 + 8192
    req(3, 1, 0, v & 0x7F, v >> 7)
    ed.close()


def post_json(url, obj):
    req = urllib.request.Request(url, data=json.dumps(obj).encode(), headers={"Content-Type": "application/json"},
                                 method="POST")
    try:
        r = urllib.request.urlopen(req, timeout=20)
        return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def settings(host, port):
    """runtime network settings (the firmware): read, refused without the current password, changed,
    back to the build's"""
    base = f"http://{host}:{port}/api/settings"
    try:
        cur = json.loads(fetch(base))
    except urllib.error.HTTPError:
        print("  (no /api/settings on this target)")
        return
    check(cur.get("source") == "build" and cur.get("password_set") is not None and "password" not in cur,
          f"settings: read ({cur.get('mode')} \"{cur.get('ssid')}\", the password never shown)")
    code, j = post_json(base, {"mode": "STA", "ssid": "Studio", "password": "studio-pass-1", "hostname": "sloop-test",
                               "current_password": "wrong-password"})
    check(code == 403, "settings: a change without the current Wi-Fi password is refused")
    code, j = post_json(base, {"mode": "STA", "ssid": "Studio", "password": "short", "hostname": "sloop-test",
                               "current_password": "change-this-password"})
    check(code == 400, f"settings: a 5-character password is refused ({j.get('error')})")
    code, j = post_json(base, {"mode": "STA", "ssid": "Studio", "password": "studio-pass-1", "hostname": "sloop-test",
                               "current_password": "change-this-password"})
    now = json.loads(fetch(base))
    check(code == 200 and now["ssid"] == "Studio" and now["hostname"] == "sloop-test" and now["source"] == "device",
          "settings: changed and kept in NVS (applies at the next boot)")
    code, j = post_json(base + "/reset", {"current_password": "studio-pass-1"})
    now = json.loads(fetch(base))
    check(code == 200 and now["source"] == "build" and now["ssid"] == cur["ssid"], "settings: back to the build's")


def run(host, port, http=True):
    if http:
        for path, want in (("/", "<html"), ("/panel.js", "createPanel"), ("/panel-geometry.js", "KEYS")):
            body = fetch(f"http://{host}:{port}{path}")
            check(want.lower() in body.decode(errors="replace").lower(), f"http: {path} is served")
    try:
        st = json.loads(fetch(f"http://{host}:{port}/api/status"))
        check("frames" in st.get("audio", {}) and "internal_min" in st.get("heap", {}) and "panels" in st.get("web", {}),
              f"status: /api/status ({st['audio']['output']} output, {st['audio']['frames']} frames, "
              f"heap internal min {st['heap']['internal_min']})")
        usb = st.get("usb_midi", {})
        check(usb.get("enabled") == 0 and usb.get("linked") == 0 and "tx_dropped" in usb,
              "status: USB-MIDI reported, off on this target (no USB device here)")
    except urllib.error.HTTPError:
        print("  (no /api/status on this target)")
    ws = WsClient(host, port)
    ws.send_json({"v": 1, "t": "hello", "client": "e2e"})
    kind, hello = ws.wait_for(is_text("hello"), 10)
    check(hello.get("proto") == 1 and hello.get("lcd") == [240, 240], "hello: protocol 1, 240x240 screen")
    check(hello.get("buttons", [])[:2] == ["FX", "SCL"] and len(hello.get("buttons", [])) == 14, "hello: 14 buttons")
    check(len(hello.get("encoders", [])) == 7 and hello.get("keys") == 27, "hello: 7 encoders, 27 keys")
    print(f"  (target {hello.get('target')}, {hello.get('sloop')}, upstream {hello.get('upstream')})")
    kind, full = ws.wait_for(lambda k, m: k == "bin", 10)
    x, y, w, h, px = parse_rect(full)
    screen = Screen()
    screen.apply(full)
    check((x, y, w, h) == (0, 0, 240, 240), "screen: the whole screen first")
    check(sum(1 for i in range(0, len(px), 2) if px[i] or px[i + 1]) > 1000, "screen: SLOOP's screen is not blank")
    kind, leds = ws.wait_for(is_text("leds"), 5)
    check(len(leds.get("s", "")) == 41 and set(leds["s"]) <= set("012"), "leds: 41 states")
    kind, st = ws.wait_for(is_text("status"), 5)
    check("playing" in st and "bpm" in st, "status: playing, bpm")
    if st.get("playing"):
        ws.send_json({"v": 1, "t": "btn", "id": "PLAY", "down": True})
        time.sleep(0.08)
        ws.send_json({"v": 1, "t": "btn", "id": "PLAY", "down": False})
        ws.wait_for(lambda k, m: k == "text" and m.get("t") == "status" and not m.get("playing"), 5)

    real_recv = ws.recv

    def recv_tracking():
        kind, msg = real_recv()
        if kind == "bin":
            screen.apply(msg)
        return kind, msg
    ws.recv = recv_tracking

    t0 = time.time()
    ws.send_json({"v": 1, "t": "btn", "id": "PLAY", "down": True})
    time.sleep(0.08)
    ws.send_json({"v": 1, "t": "btn", "id": "PLAY", "down": False})
    try:
        ws.wait_for(lambda k, m: k == "text" and m.get("t") == "status" and m.get("playing") == 1, 5)
        check(True, f"transport: PLAY tap -> playing ({(time.time() - t0) * 1000:.0f} ms)")
    except TimeoutError:
        check(False, "transport: PLAY tap -> playing")
    try:
        ws.wait_for(lambda k, m: k == "bin" and parse_rect(m)[2] < 240 or (k == "bin" and parse_rect(m)[3] < 240), 5)
        check(True, "screen: changes arrive as rectangles")
    except TimeoutError:
        check(False, "screen: changes arrive as rectangles")
    try:
        ws.wait_for(lambda k, m: k == "text" and m.get("t") == "leds" and m["s"][10] == "2", 5)
        check(True, "leds: PLAY lights while playing")
    except TimeoutError:
        check(False, "leds: PLAY lights while playing")
    ws.send_json({"v": 1, "t": "key", "k": 12, "down": True})
    ws.send_json({"v": 1, "t": "enc", "id": "K1", "d": 2})
    time.sleep(0.2)
    ws.send_json({"v": 1, "t": "key", "k": 12, "down": False})
    ws.send_json({"v": 1, "t": "btn", "id": "PLAY", "down": True})
    time.sleep(0.08)
    ws.send_json({"v": 1, "t": "btn", "id": "PLAY", "down": False})
    try:
        ws.wait_for(lambda k, m: k == "text" and m.get("t") == "status" and m.get("playing") == 0, 5)
        check(True, "transport: PLAY tap again -> stopped")
    except TimeoutError:
        check(False, "transport: PLAY tap again -> stopped")

    # the screen rebuilt from rectangles must equal a fresh whole screen
    for d in (3, -3, 2):
        ws.send_json({"v": 1, "t": "enc", "id": "SELECT", "d": d})
        time.sleep(0.25)
    end = time.time() + 1.5
    while time.time() < end:
        try:
            ws.s.settimeout(0.2)
            ws.recv()
        except Exception:  # noqa: BLE001
            pass
    ws.recv = real_recv
    ws.send_json({"v": 1, "t": "full"})
    while True:
        kind, m = ws.wait_for(lambda k, m: k == "bin", 10)
        if parse_rect(m)[2:4] == (240, 240):
            break
        screen.apply(m)
    fresh = parse_rect(m)[4]
    diff = sum(1 for i in range(0, len(fresh), 2) if fresh[i:i + 2] != screen.fb[i:i + 2])
    check(diff == 0, f"screen: {screen.rects} rectangles rebuild the exact screen ({diff} pixels differ)")

    t0 = time.time()
    ws.send_json({"v": 1, "t": "ping", "n": 4242})
    ws.wait_for(lambda k, m: k == "text" and m.get("t") == "pong" and m.get("n") == 4242, 5)
    check(True, f"ping/pong round trip {(time.time() - t0) * 1000:.1f} ms")
    ws.send_text('{"v":1,"t":"btn","id":"NOPE","down":true}')
    kind, err = ws.wait_for(is_text("err"), 5)
    check("unknown" in err.get("msg", ""), "error: an unknown button is refused with a reason")
    ws.send_text("garbage")
    ws.wait_for(is_text("err"), 5)
    ws.send_json({"v": 1, "t": "ping", "n": 1})
    ws.wait_for(lambda k, m: k == "text" and m.get("t") == "pong", 5)
    check(True, "error: the session survives malformed input")
    ws2 = WsClient(host, port)
    ws2.send_json({"v": 1, "t": "hello"})
    ws2.wait_for(is_text("hello"), 5)
    kind, full2 = ws2.wait_for(lambda k, m: k == "bin", 10)
    check(parse_rect(full2)[2:4] == (240, 240), "second panel: hello + whole screen")
    ws2.close()
    ws.close()
    editor(host, port)
    settings(host, port)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host-app")
    ap.add_argument("--web", default="web")
    ap.add_argument("--connect")
    a = ap.parse_args()
    print("web protocol e2e:")
    proc = None
    try:
        if a.connect:
            host, port = a.connect.rsplit(":", 1)
            run(host, int(port))
        else:
            s = socket.socket()
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
            s.close()
            proc = subprocess.Popen([a.host_app, "--port", str(port), "--web", a.web, "--audio", "null",
                                     "--store", "none"], stderr=subprocess.PIPE, text=True)
            line = ""
            end = time.time() + 10
            while time.time() < end and "panel at" not in line:
                line = proc.stderr.readline()
            if not re.search(r"panel at http://", line):
                print("host app did not start")
                return 1
            time.sleep(1.2)                      # the logo, then the TRACKS screen
            run("127.0.0.1", port)
    except Exception as e:  # noqa: BLE001
        check(False, f"exception: {e!r}")
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
    print("web protocol e2e:", "FAIL" if fails else "PASS")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
