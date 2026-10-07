// SPDX-License-Identifier: GPL-3.0-only
// sloopy-sloop-32 web tests (node): the panel geometry against the FM-1 and the protocol, and the
// DOM-free protocol client (drag / wheel -> detents, pot, screen messages, the session).
//   node tests/web/test_panel.mjs
import * as G from '../../web/panel-geometry.js';
import { EncoderDrag, PotDrag, parseRect, rgb565, Session, wsUrl, midiToMsg } from '../../web/proto.js';

let fails = 0;
function check(ok, what) {
  console.log(`  ${what.padEnd(72)} ${ok ? 'ok' : 'FAIL'}`);
  if (!ok) fails++;
}
const inside = (x, y, w, h, R) => x >= R.x && y >= R.y && x + w <= R.x + R.w && y + h <= R.y + R.h;
const overlap = (a, b) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
const boxOfKey = (k) => ({ x: k.x - G.KEY_W / 2, y: k.y - G.KEY_H / 2, w: G.KEY_W, h: G.KEY_H });
const boxOfBtn = (b) => ({ x: b.x - b.w / 2, y: b.y - b.h / 2, w: b.w, h: b.h });
const boxOfKnob = (k) => ({ x: k.x - k.d / 2, y: k.y - k.d / 2, w: k.d, h: k.d });

console.log('panel geometry:');
check(Math.abs(G.PANEL.w / G.PANEL.h - 1.676) < 0.01, 'panel: the FM-1 aspect ratio (1.676)');
const ids = G.BUTTONS.map((b) => b.id);
check(JSON.stringify([...ids].sort()) === JSON.stringify([...G.LED_BUTTON_ORDER].sort()) && ids.length === 14,
  'buttons: the 14 SLOOP buttons, each once');
check(G.BUTTONS.find((b) => b.id === 'SCL').print === 'SEL', "buttons: SCL is printed 'SEL' as on the FM-1");
check(G.KNOBS.map((k) => k.id).join() === 'MASTER,SELECT,PRESETS,ALGORITHM,K1,K2,K3,K4', 'knobs: MASTER + the 7 encoders');
check(G.KNOBS.filter((k) => k.kind === 'pot').length === 1, 'knobs: one pot (MASTER), the rest endless encoders');
check(G.KEYS.length === 27 && G.KEYS.filter((k) => k.white).length === 16, 'keys: 27, 16 white');
check(G.KEYS[0].midi === 53 && G.KEYS[26].midi === 79, 'keys: F3 .. G5');
check(G.KEYS.filter((k) => !k.white).map((k) => k.print).join() === 'OP1,OP2,OP3,OP4,OP5,OP6,PIT,GLO,MONO,POLY,',
  'keys: black key printing (OP1..POLY, the last blank)');
{
  const whites = G.KEYS.filter((k) => k.white);
  let ok = true;
  for (const k of G.KEYS.filter((k) => !k.white)) {
    const left = whites.filter((w) => w.midi < k.midi).pop(), right = whites.find((w) => w.midi > k.midi);
    ok = ok && Math.abs(k.x - (left.x + right.x) / 2) < 0.01;
  }
  check(ok, 'keys: every black key centred between its white neighbours');
}
check(G.KEYBED_DOTS.length === 4, 'keybed: the 4 marks between B-C and E-F');
{
  const P = { x: 0, y: 0, w: G.PANEL.w, h: G.PANEL.h };
  const all = [...G.BUTTONS.map(boxOfBtn), ...G.KEYS.map(boxOfKey), ...G.KNOBS.map(boxOfKnob),
    { ...G.SCREEN.bezel }];
  check(all.every((b) => inside(b.x, b.y, b.w, b.h, P)), 'layout: everything inside the panel');
  let clash = 0;
  for (let i = 0; i < all.length; i++) for (let j = i + 1; j < all.length; j++) if (overlap(all[i], all[j])) clash++;
  check(clash === 0, 'layout: no two controls overlap');
  const tray = (id) => G.TRAYS.find((t) => t.id === id);
  check(G.BUTTONS.slice(0, 12).every((b) => { const r = boxOfBtn(b); return inside(r.x, r.y, r.w, r.h, tray('fn')); }),
    'layout: the 12 function buttons sit in their tray');
  check(G.KEYS.every((k) => { const r = boxOfKey(k); return inside(r.x, r.y, r.w, r.h, tray('keys')); }), 'layout: the keys sit in the keybed');
  check(inside(G.SCREEN.lcd.x, G.SCREEN.lcd.y, G.SCREEN.lcd.w, G.SCREEN.lcd.h, G.SCREEN.bezel) &&
    Math.abs(G.SCREEN.lcd.w - G.SCREEN.lcd.h) < 0.01, 'screen: a square LCD inside the bezel');
}
{
  const seen = new Set(Object.values(G.KEYMAP));
  check(seen.size === 27 && [...seen].every((k) => k >= 0 && k < 27), 'shortcuts: every key once on the computer keyboard');
  const whiteRow = ['KeyZ', 'KeyX', 'KeyC', 'KeyV', 'KeyB', 'KeyN', 'KeyM', 'Comma', 'Period', 'Slash'].map((c) => G.KEYMAP[c]);
  check(whiteRow.every((k) => G.KEYS[k].white), 'shortcuts: the bottom letter row plays white keys');
}

console.log('protocol client:');
{
  const e = new EncoderDrag(10);
  e.start(100, 100);
  let s = e.move(100, 95);                  // 5 px up: not a detent yet
  s += e.move(100, 89);                     // 11 px: one
  check(s === 1, 'encoder: 10 px up = one clockwise detent, remainder kept');
  e.start(0, 0);
  check(e.move(0, 40) === -4, 'encoder: 40 px down = four counter-clockwise');
  check(e.move(100, -10000) === 64, 'encoder: a huge move is capped at 64 per message');
  check(e.wheel(-120, 0) === 2 && e.wheel(100, 1) === -1, 'encoder: wheel (pixels and lines)');
  const p = new PotDrag(724, 1.6);
  p.start(500);
  check(p.move(400) === 884, 'pot: drag up raises the value');
  check(p.move(10000) === 0 && p.move(20000) === null, 'pot: clamped at 0, no message without a change');
  check(p.set(5000) === 1023, 'pot: clamped at 1023');
}
{
  const buf = new Uint8Array(10 + 2 * 3 * 2);
  buf.set([1, 0, 5, 0, 7, 0, 2, 0, 3, 0]);
  const r = parseRect(buf.buffer);
  check(r && r.x === 5 && r.y === 7 && r.w === 2 && r.h === 3 && r.px.length === 12, 'screen: rectangle header');
  check(parseRect(new Uint8Array([1, 0, 239, 0, 0, 0, 2, 0, 1, 0, 0, 0, 0, 0])) === null, 'screen: outside the LCD -> refused');
  check(parseRect(new Uint8Array([2, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0])) === null, 'screen: another type -> refused');
  check(rgb565(0xFFFF).join() === '255,255,255' && rgb565(0xF800).join() === '255,0,0' && rgb565(0x07E0).join() === '0,255,0',
    'screen: RGB565 -> RGB');
}
{
  check(wsUrl({ protocol: 'http:', host: 'sloopy.local', search: '' }) === 'ws://sloopy.local/ws', 'session: ws URL from the page');
  check(wsUrl({ protocol: 'https:', host: 'x:8443', search: '' }) === 'wss://x:8443/ws', 'session: wss on https');
  check(wsUrl({ protocol: 'http:', host: 'a', search: '?ws=ws://b:81/ws' }) === 'ws://b:81/ws', 'session: ?ws= override');
  // a fake WebSocket: the session says hello first and routes what comes back
  const sent = [];
  class FakeWS {
    constructor() { this.readyState = 0; FakeWS.last = this; }
    send(s) { sent.push(JSON.parse(s)); }
    close() {}
  }
  const got = {};
  const s = new Session('ws://x/ws', {
    onHello: (h) => { got.hello = h; }, onLeds: (l) => { got.leds = l; }, onStatus: (st) => { got.status = st; },
    onRect: (r) => { got.rect = r; },
  }, FakeWS);
  s.connect();
  check(s.send({ v: 1, t: 'btn', id: 'PLAY', down: true }) === false, 'session: nothing is sent before the socket opens');
  FakeWS.last.readyState = 1;
  FakeWS.last.onopen();
  clearInterval(s.pinger);
  check(sent[0] && sent[0].t === 'hello' && sent[0].v === 1, 'session: hello first');
  check(!sent.some((m) => m.t === 'btn'), 'session: a press made while offline is not replayed');
  s.receive('{"v":1,"t":"hello","proto":1,"lcd":[240,240]}');
  s.receive('{"v":1,"t":"leds","s":"00000000002000000000000000000000000000000"}');
  s.receive('{"v":2,"t":"status","playing":1}');
  const rect = new Uint8Array([1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0xF8, 0x00]);
  s.receive(rect.buffer);
  check(got.hello && got.hello.proto === 1, 'session: hello delivered');
  check(got.leds && got.leds[10] === '2', 'session: LEDs delivered');
  check(!got.status, 'session: a message of another version is ignored');
  check(got.rect && got.rect.w === 1, 'session: screen rectangle delivered');
  s.closed = true;
}
{
  const on = midiToMsg(Uint8Array.from([0x92, 60, 100]));
  check(on && on.t === 'midi' && on.data.join() === '146,60,100', 'midi in: note on (channel 3)');
  check(midiToMsg(Uint8Array.from([0xC0, 5])).data.join() === '192,5', 'midi in: program change (2 bytes)');
  check(midiToMsg(Uint8Array.from([0xF8])) === null && midiToMsg(Uint8Array.from([0xFE])) === null, 'midi in: clock / active sensing dropped');
  check(midiToMsg(Uint8Array.from([0xF0, 0x7D, 0xF7])) === null, 'midi in: SysEx dropped');
  check(midiToMsg(Uint8Array.from([0x90, 200, 1])) === null && midiToMsg(Uint8Array.from([0x90, 60])) === null,
    'midi in: malformed data dropped');
}
console.log(`web panel: ${fails ? 'FAIL' : 'PASS'}`);
process.exit(fails ? 1 : 0);
