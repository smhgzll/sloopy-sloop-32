#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The panel and SLOOP's web editor in a real (headless) Chrome, driven over the DevTools protocol,
against the host app: the panel connects and shows SLOOP's screen; mouse presses on the SVG PLAY
button start and stop the transport; a held SVG button + a clicked key open SLOOP's layer (the FX
LED lights); an encoder turned with the wheel changes the tempo shown in the status bar; the editor
connects through the Web MIDI bridge and reads the device; the device page shows the status (no
firmware update on the host app).

    test_browser.py --host-app build-host/sloop_host --web web [--shots DIR]
    test_browser.py --connect 127.0.0.1:18080 --ota-image build-qemu/sloopy_sloop_32.bin
        the firmware (QEMU or a board): the device page updates the firmware with that image, as a
        user does (file, password, button), and reports the new image running (reboots the target)
    test_browser.py --connect 127.0.0.1:18080 --backup
        the device page downloads a projects backup (its link) and restores it (its form): the
        device restarts with it, the tempo is what it was (reboots the target)
Skips (exit 0) when no Chrome / Chromium is installed."""
import argparse
import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wsclient import WsClient  # noqa: E402

fails = 0


def check(ok, what):
    global fails
    print(f"  {what:<70} {'ok' if ok else 'FAIL'}")
    if not ok:
        fails += 1


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Page:
    """one Chrome tab over the DevTools protocol"""

    def __init__(self, devtools_port, url):
        req = urllib.request.Request(f"http://127.0.0.1:{devtools_port}/json/new?{url}", method="PUT")
        info = json.loads(urllib.request.urlopen(req, timeout=10).read())
        path = info["webSocketDebuggerUrl"].split(f":{devtools_port}", 1)[1]
        self.ws = WsClient("127.0.0.1", devtools_port, path)
        self.n = 0

    def call(self, method, **params):
        self.n += 1
        n = self.n
        self.ws.send_json({"id": n, "method": method, "params": params})
        kind, m = self.ws.wait_for(lambda k, m: k == "text" and m.get("id") == n, 20)
        if "error" in m:
            raise RuntimeError(f"{method}: {m['error']}")
        return m.get("result", {})

    def js(self, expr):
        r = self.call("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=True)
        if r.get("exceptionDetails"):
            raise RuntimeError(f"js: {r['exceptionDetails']}")
        return r.get("result", {}).get("value")

    def wait(self, expr, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.js(expr):
                return True
            time.sleep(0.1)
        return False

    def centre(self, selector):
        return self.js(f"(() => {{ const r = document.querySelector({json.dumps(selector)}).getBoundingClientRect();"
                       f" return [r.x + r.width / 2, r.y + r.height / 2]; }})()")

    def mouse(self, kind, x, y, button="left", buttons=1):
        self.call("Input.dispatchMouseEvent", type=kind, x=x, y=y, button=button, buttons=buttons, clickCount=1,
                  pointerType="mouse")

    def wheel(self, x, y, dy):
        self.call("Input.dispatchMouseEvent", type="mouseWheel", x=x, y=y, deltaX=0, deltaY=dy, pointerType="mouse")

    def shot(self, path):
        data = self.call("Page.captureScreenshot", format="png")["data"]
        with open(path, "wb") as f:
            f.write(base64.b64decode(data))


def find_chrome():
    for c in ("google-chrome", "chromium", "chromium-browser", "google-chrome-stable"):
        p = shutil.which(c)
        if p:
            return p
    return None


def start_chrome(chrome):
    """headless Chrome with a throwaway profile: (process, profile dir, DevTools port)"""
    dport = free_port()
    prof = tempfile.mkdtemp(prefix="sloopy-chrome-")
    br = subprocess.Popen([chrome, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
                           f"--user-data-dir={prof}", f"--remote-debugging-port={dport}", "--window-size=1300,860",
                           "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + 15
    while time.time() < end:
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{dport}/json/version", timeout=1)
            break
        except OSError:
            time.sleep(0.2)
    return br, prof, dport


def device_backup(chrome, target, password):
    """the device page's projects backup: download it with the link, restore it with the form"""
    print("browser: projects backup from the device page:")
    br, prof, dport = start_chrome(chrome)
    dl = tempfile.mkdtemp(prefix="sloopy-dl-")
    try:
        info = json.loads(urllib.request.urlopen(f"http://127.0.0.1:{dport}/json/version", timeout=5).read())
        bws = WsClient("127.0.0.1", dport, info["webSocketDebuggerUrl"].split(f":{dport}", 1)[1])
        bws.send_json({"id": 1, "method": "Browser.setDownloadBehavior",
                       "params": {"behavior": "allow", "downloadPath": dl}})
        bws.wait_for(lambda k, m: k == "text" and m.get("id") == 1, 10)
        bpm = json.loads(urllib.request.urlopen(f"http://{target}/api/status", timeout=10).read())["bpm"]
        pg = Page(dport, f"http://{target}/device.html")
        pg.call("Page.enable")
        ok = pg.wait("document.getElementById('bk') && !document.getElementById('bk').hidden", 15)
        check(ok, "device page: the projects backup section is shown")
        pg.js("document.getElementById('bk-get').click()")
        end, files = time.time() + 60, []
        while time.time() < end:
            files = [f for f in os.listdir(dl) if f.startswith("sloop-store-") and f.endswith(".bin")]
            if files and os.path.getsize(os.path.join(dl, files[0])) == 0x70000:
                break
            time.sleep(0.3)
        path = os.path.join(dl, files[0]) if files else None
        check(path is not None and os.path.getsize(path) == 0x70000,
              f"device page: the backup link saves {files[0] if files else 'nothing'} (448 KiB)")
        if path:
            root = pg.call("DOM.getDocument")["root"]["nodeId"]
            node = pg.call("DOM.querySelector", nodeId=root, selector="#bkf input[name=image]")["nodeId"]
            pg.call("DOM.setFileInputFiles", nodeId=node, files=[path])
            pg.js(f"document.querySelector('#bkf input[name=current_password]').value = {json.dumps(password)}")
            pg.js("document.querySelector('#bkf button[type=submit]').click()")
            ok = pg.wait("/^Restored/.test(document.getElementById('bk-msg').textContent)", 120)
            check(ok, f"device page: {pg.js('document.getElementById(\"bk-msg\").textContent')}")
            after = json.loads(urllib.request.urlopen(f"http://{target}/api/status", timeout=10).read())["bpm"]
            check(after == bpm, f"after the restore: the tempo as saved ({bpm} -> {after})")
        bws.close()
    except Exception as e:  # noqa: BLE001
        check(False, f"exception: {e!r}")
    finally:
        br.terminate()
        br.wait(5)
        shutil.rmtree(prof, ignore_errors=True)
        shutil.rmtree(dl, ignore_errors=True)
    print("browser:", "FAIL" if fails else "PASS")
    return 1 if fails else 0


def device_update(chrome, target, image, password):
    """the device page's firmware update, as a user does it, against the firmware"""
    print("browser: firmware update from the device page:")
    br, prof, dport = start_chrome(chrome)
    try:
        pg = Page(dport, f"http://{target}/device.html")
        pg.call("Page.enable")
        ok = pg.wait("document.getElementById('fw') && !document.getElementById('fw').hidden", 15)
        before = pg.js("document.getElementById('fw-now').textContent")
        check(ok, f"device page: the firmware section is shown ({before})")
        root = pg.call("DOM.getDocument")["root"]["nodeId"]
        node = pg.call("DOM.querySelector", nodeId=root, selector="#fwf input[name=image]")["nodeId"]
        pg.call("DOM.setFileInputFiles", nodeId=node, files=[os.path.abspath(image)])
        pg.js(f"document.querySelector('#fwf input[name=current_password]').value = {json.dumps(password)}")
        t0 = time.time()
        pg.js("document.querySelector('#fwf button[type=submit]').click()")
        ok = pg.wait("/^Written to ota_/.test(document.getElementById('fw-msg').textContent)", 120)
        check(ok, f"device page: the image is sent ({pg.js('document.getElementById(\"fw-msg\").textContent')},"
                  f" {time.time() - t0:.1f} s)")
        ok = pg.wait("/^Updated: running/.test(document.getElementById('fw-msg').textContent)", 180)
        check(ok, f"device page: {pg.js('document.getElementById(\"fw-msg\").textContent')}")
    except Exception as e:  # noqa: BLE001
        check(False, f"exception: {e!r}")
    finally:
        br.terminate()
        br.wait(5)
        shutil.rmtree(prof, ignore_errors=True)
    print("browser:", "FAIL" if fails else "PASS")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host-app")
    ap.add_argument("--web", default="web")
    ap.add_argument("--shots")
    ap.add_argument("--connect", help="host:port of a running firmware (with --ota-image)")
    ap.add_argument("--ota-image")
    ap.add_argument("--backup", action="store_true")
    ap.add_argument("--password", default="change-this-password")   # the build's placeholder
    a = ap.parse_args()
    chrome = find_chrome()
    if not chrome:
        print("browser:\n  no Chrome / Chromium: skipped")
        return 0
    if a.connect and a.backup:
        return device_backup(chrome, a.connect, a.password)
    if a.connect:
        return device_update(chrome, a.connect, a.ota_image, a.password)
    print("browser:")
    port = free_port()
    host = subprocess.Popen([a.host_app, "--port", str(port), "--web", a.web, "--audio", "null", "--store", "none"],
                            stderr=subprocess.DEVNULL)
    br, prof, dport = start_chrome(chrome)
    try:
        time.sleep(1.0)                                   # (the host app: logo, then TRACKS)
        pg = Page(dport, f"http://127.0.0.1:{port}/")
        pg.call("Page.enable")
        check(pg.wait("document.querySelector('#conn.open') !== null"), "panel: connects to SLOOP")
        lit = pg.wait("(() => { const c = document.querySelector('canvas.fm1-lcd'); const d = c.getContext('2d')"
                      ".getImageData(0, 0, 240, 240).data; let n = 0; for (let i = 0; i < d.length; i += 4)"
                      " n += d[i] + d[i + 1] + d[i + 2] > 60; return n > 2000; })()")
        check(lit, "panel: SLOOP's screen is drawn in the LCD canvas")
        check(pg.js("document.querySelectorAll('.ctl.btn').length") == 14 and
              pg.js("document.querySelectorAll('.ctl.key').length") == 27 and
              pg.js("document.querySelectorAll('.ctl.knob').length") == 8, "panel: 14 buttons, 27 keys, 8 knobs")

        # PLAY with the mouse on the SVG button
        x, y = pg.centre(".btn[data-id=PLAY]")
        pg.mouse("mousePressed", x, y)
        time.sleep(0.08)
        pg.mouse("mouseReleased", x, y, buttons=0)
        check(pg.wait("document.getElementById('info').textContent.includes('▶')"), "mouse: PLAY starts the transport")
        check(pg.wait("document.querySelector('.btn[data-id=PLAY]').classList.contains('lit')", 3),
              "LEDs: the PLAY button flashes")
        pg.mouse("mousePressed", x, y)
        time.sleep(0.08)
        pg.mouse("mouseReleased", x, y, buttons=0)
        check(pg.wait("document.getElementById('info').textContent.includes('■')"), "mouse: PLAY again stops it")

        # right-click latches FX (a mouse "holds" it), then a key: SLOOP's FX layer
        fx = pg.centre(".btn[data-id=FX]")
        pg.mouse("mousePressed", *fx, button="right", buttons=2)
        pg.mouse("mouseReleased", *fx, button="right", buttons=0)
        check(pg.wait("document.querySelector('.btn[data-id=FX]').classList.contains('latched')", 2),
              "latch: right-click holds FX down")
        check(pg.wait("document.querySelector('.btn[data-id=FX]').classList.contains('lit')", 3),
              "latch: SLOOP lights FX (its layer is open)")
        check(pg.wait("document.querySelector('.key[data-k=\"0\"]').classList.contains('dim')", 3),
              "layer: the landmark keys glow dimly (keys 1, 5, 9, 13)")
        if a.shots:
            os.makedirs(a.shots, exist_ok=True)
            pg.shot(os.path.join(a.shots, "browser-fx-layer.png"))
        pg.call("Input.dispatchKeyEvent", type="keyDown", key="Escape", code="Escape", windowsVirtualKeyCode=27)
        pg.call("Input.dispatchKeyEvent", type="keyUp", key="Escape", code="Escape", windowsVirtualKeyCode=27)
        check(pg.wait("!document.querySelector('.btn[data-id=FX]').classList.contains('latched')", 2),
              "latch: Esc lets go")

        # multi-touch: one finger holds FX, another touches white key 5: SLOOP's punch-in lights it
        pg.call("Emulation.setTouchEmulationEnabled", enabled=True, maxTouchPoints=5)
        fx = pg.centre(".btn[data-id=FX]")
        k5 = pg.centre('.key[data-k="7"]')                # (white key 5 = C4)
        pg.call("Input.dispatchTouchEvent", type="touchStart", touchPoints=[{"x": fx[0], "y": fx[1], "id": 1}])
        time.sleep(0.3)
        pg.call("Input.dispatchTouchEvent", type="touchStart",
                touchPoints=[{"x": fx[0], "y": fx[1], "id": 1}, {"x": k5[0], "y": k5[1], "id": 2}])
        lit5 = pg.wait("document.querySelector('.key[data-k=\"7\"]').classList.contains('lit') && "
                       "document.querySelector('.btn[data-id=FX]').classList.contains('lit')", 3)
        check(lit5, "touch: FX held by one finger + key 5 by another: SLOOP's punch-in is on")
        pg.call("Input.dispatchTouchEvent", type="touchEnd", touchPoints=[{"x": fx[0], "y": fx[1], "id": 1}])
        pg.call("Input.dispatchTouchEvent", type="touchEnd", touchPoints=[])
        check(pg.wait("!document.querySelector('.btn[data-id=FX]').classList.contains('down') && "
                      "!document.querySelector('.key[data-k=\"7\"]').classList.contains('down')", 3),
              "touch: lifting the fingers releases both")
        pg.call("Emulation.setTouchEmulationEnabled", enabled=False)

        # the wheel on SELECT: the tempo
        bpm0 = pg.js("(document.getElementById('info').textContent.match(/(\\d+) bpm/) || [0, 0])[1] | 0")
        sx, sy = pg.centre(".knob[data-id=SELECT]")
        for _ in range(3):
            pg.wheel(sx, sy, -100)                        # (deltaMode pixels: 2 detents each)
            time.sleep(0.15)
        check(pg.wait(f"(document.getElementById('info').textContent.match(/(\\d+) bpm/) || [0, 0])[1] > {bpm0}"),
              "wheel: SELECT raises the tempo")
        if a.shots:
            pg.shot(os.path.join(a.shots, "browser-panel.png"))

        # a phone in portrait: the panel keeps its proportions inside the screen, a hint to rotate
        pg.call("Emulation.setDeviceMetricsOverride", width=390, height=844, deviceScaleFactor=3, mobile=True)
        time.sleep(0.4)
        geo = pg.js("(() => { const r = document.getElementById('panel').getBoundingClientRect();"
                    " return [r.width, r.height, innerWidth, getComputedStyle(document.getElementById('rotate-hint')).display]; })()")
        check(abs(geo[0] / geo[1] - 1.676) < 0.02 and geo[0] <= geo[2], f"phone: panel {geo[0]:.0f} x {geo[1]:.0f}, ratio kept, fits")
        check(geo[3] != "none", "phone: portrait shows the rotate hint")
        if a.shots:
            pg.shot(os.path.join(a.shots, "browser-phone-portrait.png"))
        pg.call("Emulation.setDeviceMetricsOverride", width=844, height=390, deviceScaleFactor=3, mobile=True)
        time.sleep(0.4)
        geo = pg.js("(() => { const r = document.getElementById('panel').getBoundingClientRect();"
                    " return [r.width, r.height, innerHeight, getComputedStyle(document.getElementById('rotate-hint')).display]; })()")
        check(geo[1] <= geo[2] and geo[3] == "none" and abs(geo[0] / geo[1] - 1.676) < 0.02,
              f"phone: landscape panel {geo[0]:.0f} x {geo[1]:.0f} fits the height, no hint")
        if a.shots:
            pg.shot(os.path.join(a.shots, "browser-phone-landscape.png"))
        pg.call("Emulation.clearDeviceMetricsOverride")

        # SLOOP's web editor through the Web MIDI bridge
        ed = Page(dport, f"http://127.0.0.1:{port}/editor.html")
        ed.call("Page.enable")
        ed.wait("document.readyState === 'complete'")
        ed.wait("window.sloopyEditorBridge && window.sloopyEditorBridge.input.state === 'connected'")
        ed.js("document.getElementById('connect').click()")
        ok = ed.wait("/SLOOP|FELUCCA/i.test(document.getElementById('version').textContent)", 10)
        check(ok, f"editor: connects over the bridge ({ed.js('document.getElementById(\"version\").textContent')})")
        t0 = time.time()
        loaded = ed.wait("!/Reading \\d+\\/\\d+/.test(document.body.innerText) && "
                         "!/Connect the FM-1 to edit/.test(document.body.innerText)", 30)
        check(loaded, f"editor: reads every parameter of the device ({time.time() - t0:.1f} s)")
        check(ed.js("document.querySelectorAll('input[type=range], select').length") > 10,
              "editor: the sound's parameters are shown")
        if a.shots:
            time.sleep(0.5)
            ed.shot(os.path.join(a.shots, "browser-editor.png"))

        # the device page: status; no firmware update on the host app
        dev = Page(dport, f"http://127.0.0.1:{port}/device.html")
        dev.call("Page.enable")
        ok = dev.wait("/firmware/.test(document.getElementById('status').innerText) && "
                      "/USB-MIDI/.test(document.getElementById('status').innerText)", 10)
        check(ok and dev.js("document.getElementById('fw').hidden"),
              "device page: status shown; no firmware update on the host app")
    except Exception as e:  # noqa: BLE001
        check(False, f"exception: {e!r}")
    finally:
        br.terminate()
        host.terminate()
        br.wait(5)
        host.wait(5)
        shutil.rmtree(prof, ignore_errors=True)
    print("browser:", "FAIL" if fails else "PASS")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
