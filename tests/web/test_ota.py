#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Firmware update from the browser (POST /api/ota) against the firmware (QEMU or a board):

    test_ota.py --connect 127.0.0.1:18080 --image build-qemu/sloopy_sloop_32.bin [--log qemu.log]

Refused: the wrong password, a body that is no ESP-IDF image, another project's image, a truncated
image (it does not verify); the device keeps running its image. Then the real image: it is written
into the other OTA slot, the device restarts into it, the new image confirms itself (SLOOP runs,
the web server is up: no rollback) and the panel answers on the WebSocket. This reboots the target."""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wsclient import WsClient  # noqa: E402

fails = 0


def check(ok, what):
    global fails
    print(f"  {what:<70} {'ok' if ok else 'FAIL'}")
    if not ok:
        fails += 1


def status(base, timeout=10):
    return json.loads(urllib.request.urlopen(f"{base}/api/status", timeout=timeout).read())


def upload(base, body, password, timeout=300):
    req = urllib.request.Request(f"{base}/api/ota", data=body, method="POST",
                                 headers={"Content-Type": "application/octet-stream",
                                          "X-Sloopy-Password": urllib.parse.quote(password, safe="")})
    try:
        r = urllib.request.urlopen(req, timeout=timeout)
        return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--connect", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--password", default="change-this-password")   # the build's placeholder
    ap.add_argument("--log", help="the device's console log, to check the boot markers")
    a = ap.parse_args()
    host, port = a.connect.rsplit(":", 1)
    base = f"http://{host}:{port}"
    image = open(a.image, "rb").read()
    print("firmware update (POST /api/ota):")

    before = status(base)
    app = before.get("app", {})
    check(app.get("ota") == 1 and app.get("partition") in ("ota_0", "ota_1"),
          f"status: running from {app.get('partition')} ({app.get('ota_state')}), version {app.get('version')}")
    slot = app.get("partition")
    other = "ota_1" if slot == "ota_0" else "ota_0"

    code, j = upload(base, image[:4096], "wrong-password")
    check(code == 403, "refused: the wrong Wi-Fi password")
    code, j = upload(base, bytes(16384), a.password)
    check(code == 400 and "not an ESP-IDF" in j.get("error", ""), f"refused: not an image ({j.get('error')})")
    patched = bytearray(image[:65536])
    patched[80:80 + 32] = b"another_project".ljust(32, b"\0")   # esp_app_desc_t.project_name
    code, j = upload(base, bytes(patched), a.password)
    check(code == 400 and "another project" in j.get("error", ""), f"refused: another project ({j.get('error')})")
    t0 = time.time()
    code, j = upload(base, image[:len(image) // 3], a.password)
    check(code == 400 and "does not verify" in j.get("error", ""),
          f"refused: a truncated image ({j.get('error')}, {time.time() - t0:.1f} s)")
    now = status(base)
    check(now["app"]["partition"] == slot and now["boot"] == before["boot"],
          "after the refusals: same image, no restart")

    t0 = time.time()
    code, j = upload(base, image, a.password)
    took = time.time() - t0
    check(code == 200 and j.get("restart") and j.get("partition") == other,
          f"the image: written to {j.get('partition')} ({len(image) // 1024} KB in {took:.1f} s), restarting")
    after, end = None, time.time() + 180
    time.sleep(3)
    while time.time() < end:
        try:
            s = status(base, timeout=5)
            if s["app"]["partition"] == other and s["app"]["ota_state"] != "pending":
                after = s
                break
        except (urllib.error.URLError, ConnectionError, OSError, ValueError, KeyError):
            pass
        time.sleep(1)
    check(after is not None and after["app"]["ota_state"] == "valid",
          f"restarted into {other}, confirmed ({(after or {}).get('app', {}).get('ota_state')})")
    check(after is not None and after["boot"] != before["boot"], "it is a new boot")
    if after is not None:
        ws = WsClient(host, int(port))
        ws.send_json({"v": 1, "t": "hello", "client": "ota-test"})
        kind, hello = ws.wait_for(lambda k, m: k == "text" and m.get("t") == "hello", 10)
        check(hello.get("proto") == 1, "the panel answers after the update")
        ws.close()
    if a.log:
        log = open(a.log, errors="replace").read()
        check(f"SLOOPY_OTA_CONFIRMED {other}" in log and "SLOOPY_OTA_BOOT" in log,
              "console: the update's first boot confirmed it, self-test skipped")
    print(f"firmware update: {'PASS' if not fails else 'FAIL'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
