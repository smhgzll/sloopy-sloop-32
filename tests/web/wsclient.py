# SPDX-License-Identifier: GPL-3.0-only
"""A minimal blocking WebSocket client (RFC 6455) for the panel protocol tests: no packages
needed beyond the Python standard library."""
import base64
import hashlib
import json
import os
import socket
import struct
import time


class WsClient:
    def __init__(self, host, port, path="/ws", timeout=10.0):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)   # (as browsers do for WebSockets)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.s.sendall(req.encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.s.recv(4096)
            if not chunk:
                raise ConnectionError("closed during the handshake")
            resp += chunk
        head, self.buf = resp.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise ConnectionError("no upgrade: " + head.decode(errors="replace"))
        want = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        if b"Sec-WebSocket-Accept: " + want not in head:
            raise ConnectionError("bad Sec-WebSocket-Accept")

    def _send(self, op, payload):
        mask = os.urandom(4)
        n = len(payload)
        hdr = bytes([0x80 | op])
        if n < 126:
            hdr += bytes([0x80 | n])
        elif n < 65536:
            hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
        self.s.sendall(hdr + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(payload)))

    def send_json(self, obj):
        self._send(0x1, json.dumps(obj, separators=(",", ":")).encode())

    def send_text(self, s):
        self._send(0x1, s.encode())

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(262144)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk

    def recv(self):
        """next message: ("text", dict) or ("bin", bytes)"""
        while True:
            self._need(2)
            op = self.buf[0] & 0x0F
            n = self.buf[1] & 0x7F
            h = 2
            if n == 126:
                self._need(4)
                n = struct.unpack(">H", self.buf[2:4])[0]
                h = 4
            elif n == 127:
                self._need(10)
                n = struct.unpack(">Q", self.buf[2:10])[0]
                h = 10
            self._need(h + n)
            payload, self.buf = self.buf[h:h + n], self.buf[h + n:]
            if op == 0x1:
                return "text", json.loads(payload.decode())
            if op == 0x2:
                return "bin", payload
            if op == 0x8:
                raise ConnectionError("closed by the server")

    def wait_for(self, pred, timeout=5.0):
        """receive until pred(kind, msg) is true; returns that message (or raises TimeoutError)"""
        end = time.time() + timeout
        while time.time() < end:
            self.s.settimeout(max(0.05, end - time.time()))
            try:
                kind, msg = self.recv()
            except socket.timeout:
                continue
            if pred(kind, msg):
                return kind, msg
        raise TimeoutError("no matching message")

    def close(self):
        try:
            self._send(0x8, b"\x03\xe8")
        finally:
            self.s.close()


def parse_rect(b):
    """binary display message -> (x, y, w, h, pixels)"""
    assert b[0] == 0x01 and b[1] == 0x00, "display rect, RGB565 BE"
    x, y, w, h = struct.unpack("<HHHH", b[2:10])
    assert len(b) == 10 + w * h * 2, "rect size"
    return x, y, w, h, b[10:]
