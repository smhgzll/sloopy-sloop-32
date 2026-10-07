// SPDX-License-Identifier: GPL-3.0-only
// sloopy-sloop-32: the browser side of the panel protocol (docs/PROTOCOL.md), DOM-free so node can
// test it: drag / wheel -> encoder detents and pot values, the binary screen message, and a
// WebSocket session that reconnects by itself.

export const PROTO_VERSION = 1;

// vertical drag (up = clockwise) and wheel -> whole detent steps; the remainder carries over
export class EncoderDrag {
  constructor(pxPerStep = 10) { this.px = pxPerStep; this.acc = 0; this.last = null; this.wheelAcc = 0; }
  start(x, y) { this.last = { x, y }; this.acc = 0; }
  move(x, y) {
    if (!this.last) return 0;
    // up and right turn clockwise (a thumb on a touch screen moves either way)
    this.acc += (this.last.y - y) + (x - this.last.x) * 0.5;
    this.last = { x, y };
    const steps = Math.trunc(this.acc / this.px);
    this.acc -= steps * this.px;
    return Math.max(-64, Math.min(64, steps));
  }
  end() { this.last = null; this.acc = 0; }
  // deltaMode 0: pixels (trackpads: many small deltas), 1: lines, 2: pages; wheel down = counter-clockwise
  wheel(deltaY, deltaMode = 0) {
    if (deltaMode === 0) {
      this.wheelAcc += -deltaY;
      const steps = Math.trunc(this.wheelAcc / 50);
      this.wheelAcc -= steps * 50;
      return Math.max(-64, Math.min(64, steps));
    }
    return deltaY < 0 ? 1 : deltaY > 0 ? -1 : 0;
  }
}

// the MASTER pot: 0..1023, a vertical drag of 1023 / unitsPerPx pixels covers the range
export class PotDrag {
  constructor(value = 724, unitsPerPx = 1.6) { this.value = value; this.k = unitsPerPx; this.y = null; this.acc = 0; }
  set(v) {
    const nv = Math.max(0, Math.min(1023, Math.round(v)));
    if (nv === this.value) return null;
    this.value = nv;
    return nv;
  }
  start(y) { this.y = y; this.acc = this.value; }
  move(y) {
    if (this.y === null) return null;
    this.acc = Math.max(0, Math.min(1023, this.acc + (this.y - y) * this.k));
    this.y = y;
    return this.set(this.acc);
  }
  wheel(deltaY) { return this.set(this.value + (deltaY < 0 ? 24 : -24)); }
}

// binary display message -> { x, y, w, h, px } (px: RGB565 big-endian bytes), null if not one
export function parseRect(buf) {
  const b = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  if (b.length < 10 || b[0] !== 0x01 || b[1] !== 0x00) return null;
  const x = b[2] | (b[3] << 8), y = b[4] | (b[5] << 8), w = b[6] | (b[7] << 8), h = b[8] | (b[9] << 8);
  if (x + w > 240 || y + h > 240 || b.length !== 10 + w * h * 2) return null;
  return { x, y, w, h, px: b.subarray(10) };
}

// RGB565 -> 8-bit channels, as the panel draws them
export function rgb565(v) {
  return [((v >> 11) * 527 + 23) >> 6, (((v >> 5) & 63) * 259 + 33) >> 6, ((v & 31) * 527 + 23) >> 6];
}

// a Web MIDI message (MIDIMessageEvent.data) -> the protocol's "midi" message, or null: channel
// messages only (note off / on, poly pressure, CC, program, channel pressure, pitch bend); clock,
// active sensing, SysEx and malformed data are dropped
export function midiToMsg(data) {
  if (!data || data.length < 2) return null;
  const st = data[0];
  if (st < 0x80 || st >= 0xF0) return null;
  const n = (st & 0xF0) === 0xC0 || (st & 0xF0) === 0xD0 ? 2 : 3;
  if (data.length < n) return null;
  for (let i = 1; i < n; i++) if (data[i] > 127) return null;
  return { v: PROTO_VERSION, t: 'midi', data: Array.from(data.slice(0, n)) };
}

export function wsUrl(loc) {
  const q = new URLSearchParams(loc.search || '');
  if (q.get('ws')) return q.get('ws');
  return `${loc.protocol === 'https:' ? 'wss' : 'ws'}://${loc.host}/ws`;
}

// a session that keeps itself connected. handlers: onHello, onLeds, onStatus, onRect, onState, onError
export class Session {
  constructor(url, handlers, WS = globalThis.WebSocket) {
    this.url = url; this.h = handlers; this.WS = WS; this.ws = null; this.delay = 500;
    this.queue = []; this.rtt = null; this.pingN = 0; this.pingT = new Map(); this.closed = false;
    this.stats = { rx: 0, rects: 0, bytes: 0 };
  }
  connect() {
    if (this.closed) return;
    this.h.onState && this.h.onState('connecting');
    const ws = new this.WS(this.url);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onopen = () => {
      this.delay = 500;
      this.send({ v: PROTO_VERSION, t: 'hello', client: 'fm1-panel' });
      for (const m of this.queue.splice(0)) this.send(m);
      this.h.onState && this.h.onState('open');
      this.pinger = setInterval(() => this.ping(), 2000);
    };
    ws.onmessage = (ev) => this.receive(ev.data);
    ws.onclose = () => {
      clearInterval(this.pinger);
      this.h.onState && this.h.onState('closed');
      if (!this.closed) setTimeout(() => this.connect(), this.delay);
      this.delay = Math.min(this.delay * 2, 5000);
    };
    ws.onerror = () => {};
  }
  receive(data) {
    this.stats.rx++;
    if (typeof data === 'string') {
      let m;
      try { m = JSON.parse(data); } catch { return; }
      if (m.v !== PROTO_VERSION) return;
      if (m.t === 'hello') this.h.onHello && this.h.onHello(m);
      else if (m.t === 'leds') this.h.onLeds && this.h.onLeds(m.s);
      else if (m.t === 'status') this.h.onStatus && this.h.onStatus(m);
      else if (m.t === 'err') this.h.onError && this.h.onError(m.msg);
      else if (m.t === 'pong' && this.pingT.has(m.n)) {
        this.rtt = Date.now() - this.pingT.get(m.n);
        this.pingT.delete(m.n);
        this.h.onRtt && this.h.onRtt(this.rtt);
      }
      return;
    }
    const r = parseRect(data);
    if (r) {
      this.stats.rects++;
      this.stats.bytes += data.byteLength;
      this.h.onRect && this.h.onRect(r);
    }
  }
  send(msg) {
    if (!this.ws || this.ws.readyState !== 1) {
      // controls pressed while reconnecting are dropped (a release without its press would be wrong)
      if (msg.t === 'pot') this.queue = this.queue.filter((m) => m.t !== 'pot').concat([msg]);
      return false;
    }
    this.ws.send(JSON.stringify(msg));
    return true;
  }
  ping() {
    const n = ++this.pingN;
    this.pingT.set(n, Date.now());
    if (this.pingT.size > 8) this.pingT.delete(this.pingT.keys().next().value);
    this.send({ v: PROTO_VERSION, t: 'ping', n });
  }
  close() { this.closed = true; if (this.ws) this.ws.close(); }
}
