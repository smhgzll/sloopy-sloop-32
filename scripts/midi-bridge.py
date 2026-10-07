#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A MIDI keyboard on this computer plays the board, no browser involved (Linux, ALSA):

    scripts/midi-bridge.py [--device sloopy.local] [--port 24:0] [--follow]

Reads the keyboard's ALSA sequencer port with `aseqdump` (alsa-utils) and sends each channel
message to the board over the panel's WebSocket ({"t": "midi"}, as the panel's "midi in" does).
SLOOP's MIDI in (seq.c midi_route): channels 1-3 play the synth tracks, 10 the drums, any other
channel the track selected on the panel. --follow sends everything but channel 10 on channel 16:
the keyboard then plays the selected track, as the panel's keys do. Without --port every hardware
MIDI input is read. Leave the panel's "midi in" off meanwhile, or notes arrive twice. Ctrl-C stops."""
import argparse
import os
import re
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "web"))
from wsclient import WsClient  # noqa: E402

EVENTS = {   # aseqdump's name -> (status nibble, the fields after the channel)
    "Note on": (0x90, ("note", "velocity")),
    "Note off": (0x80, ("note", "velocity")),
    "Polyphonic aftertouch": (0xA0, ("note", "value")),
    "Control change": (0xB0, ("controller", "value")),
    "Program change": (0xC0, ("program",)),
    "Channel aftertouch": (0xD0, ("value",)),
    "Pitch bend": (0xE0, ("value",)),
}
LINE = re.compile(r"^\s*\d+:\d+\s+(" + "|".join(EVENTS) + r")\s+(\d+),\s*(.*)$")


def to_bytes(line):
    m = LINE.match(line)
    if not m:
        return None
    status, names = EVENTS[m.group(1)]
    fields = dict(re.findall(r"(\w+) (-?\d+)", m.group(3)))
    st = status | int(m.group(2)) & 0x0F
    if status == 0xE0:                                   # aseqdump prints the bend as -8192..8191
        v = int(fields["value"]) + 8192
        return [st, v & 0x7F, v >> 7 & 0x7F]
    return [st] + [int(fields[n]) & 0x7F for n in names]


def hardware_ports():
    out = subprocess.run(["aseqdump", "-l"], capture_output=True, text=True, check=True).stdout
    ports = []
    for line in out.splitlines()[1:]:
        m = re.match(r"\s*(\d+:\d+)\s+(.*?)\s{2,}", line)
        if m and not re.match(r"System|Midi Through|PipeWire", m.group(2)):
            ports.append((m.group(1), m.group(2)))
    return ports


def connect(host, port):
    ws = WsClient(host, port)
    ws.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)   # (a note per frame, sent now)
    ws.send_json({"v": 1, "t": "hello", "client": "midi-bridge", "screen": False})
    ws.wait_for(lambda k, m: k == "text" and m.get("t") == "hello", 5)

    def drain():                                         # (the board's state messages: not needed)
        while True:
            try:
                ws.recv()
            except socket.timeout:
                continue
            except Exception:
                return
    threading.Thread(target=drain, daemon=True).start()
    return ws


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default="sloopy.local", help="the board's address (default sloopy.local)")
    ap.add_argument("--port", help="ALSA sequencer port(s), e.g. 24:0 (default: every hardware input)")
    ap.add_argument("--follow", action="store_true",
                    help="play the track selected on the panel (all but channel 10 -> channel 16)")
    a = ap.parse_args()
    host, _, wport = a.device.partition(":")
    ports = a.port or ",".join(p for p, _ in hardware_ports())
    if not ports:
        sys.exit("no MIDI keyboard found (aseqdump -l)")
    print(f"MIDI from {ports} -> ws://{a.device}/ws{'  (the selected track)' if a.follow else ''}"
          "   (Ctrl-C stops)", flush=True)
    ws = None
    # line-buffered: into a pipe, aseqdump's stdio can hold an event back (~0.5 s measured)
    dump = subprocess.Popen(["stdbuf", "-oL", "aseqdump", "-p", ports], stdout=subprocess.PIPE, text=True,
                            bufsize=1)
    try:
        for line in dump.stdout:
            data = to_bytes(line)
            if data is None:
                continue
            if a.follow and data[0] & 0x0F != 9:
                data[0] |= 0x0F
            for attempt in (1, 2):
                try:
                    if ws is None:
                        ws = connect(host, int(wport or 80))
                        print("connected to the board", flush=True)
                    ws.send_json({"v": 1, "t": "midi", "data": data})
                    break
                except OSError as e:
                    print(f"board: {e}; reconnecting", flush=True)
                    ws = None
                    time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        dump.terminate()


if __name__ == "__main__":
    main()
