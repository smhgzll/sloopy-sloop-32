// SPDX-License-Identifier: GPL-3.0-only
// sloopy-sloop-32: the FM-1 front panel in the browser: SVG controls built from panel-geometry.js,
// SLOOP's screen in a canvas over the LCD. The panel only reports what a hand does (a button goes
// down / up, a key, encoder detents, the MASTER pot); SLOOP decides what it means (holds, layers,
// long presses, combinations), exactly as with the physical panel.
//
//   const panel = createPanel(container, { send })      send(msg): a protocol message object
//   panel.setLeds("0120...")  panel.drawRect(x, y, w, h, rgb565be)  panel.reset()
//
// Interaction: pointer (mouse, touch, pen; multi-touch: hold a layer button and play keys),
// wheel and drag on encoders, drag / wheel on MASTER, right-click latches a button or key (for a
// mouse: "hold FX" while clicking keys; Esc releases all latches), computer keyboard shortcuts.

import {
  PANEL, SCREEN, KNOBS, KNOB_LABEL_DY, TRAYS, BUTTONS, FN_DOT, KEYS, KEY_W, KEY_H, KEYBED_DOTS,
  LED_BUTTON_ORDER, KEYMAP, BUTTON_KEYMAP, noteName,
} from './panel-geometry.js';
import { EncoderDrag, PotDrag } from './proto.js';

const NS = 'http://www.w3.org/2000/svg';
function el(tag, attrs = {}, parent) {
  const e = document.createElementNS(NS, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  if (parent) parent.appendChild(e);
  return e;
}
const LABEL_FONT = 'Bahnschrift, "DIN Alternate", "Roboto Condensed", "Arial Narrow", system-ui, sans-serif';

function defs(svg) {
  const d = el('defs', {}, svg);
  d.innerHTML = `
    <linearGradient id="g-body" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#47484b"/><stop offset="0.45" stop-color="#545558"/>
      <stop offset="1" stop-color="#4b4c4f"/></linearGradient>
    <linearGradient id="g-sheen" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#000" stop-opacity="0.10"/><stop offset="0.35" stop-color="#fff" stop-opacity="0.05"/>
      <stop offset="0.7" stop-color="#000" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity="0.22"/></linearGradient>
    <linearGradient id="g-tray" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#2c2c2e"/><stop offset="1" stop-color="#363638"/></linearGradient>
    <linearGradient id="g-btn" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#2f2f31"/><stop offset="1" stop-color="#1f1f21"/></linearGradient>
    <linearGradient id="g-key" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#323234"/><stop offset="1" stop-color="#232325"/></linearGradient>
    <radialGradient id="g-knob" cx="0.42" cy="0.36" r="0.7">
      <stop offset="0" stop-color="#4a4a4c"/><stop offset="0.6" stop-color="#2a2a2c"/><stop offset="1" stop-color="#18181a"/></radialGradient>
    <radialGradient id="g-knob-top" cx="0.45" cy="0.4" r="0.65">
      <stop offset="0" stop-color="#3a3a3c"/><stop offset="1" stop-color="#232325"/></radialGradient>
    <filter id="f-led" x="-2" y="-0.6" width="5" height="2.2"><feGaussianBlur stdDeviation="2.4"/></filter>
    <filter id="f-btnled" x="-0.5" y="-0.5" width="2" height="2"><feGaussianBlur stdDeviation="3.5"/></filter>
    <filter id="f-shadow" x="-0.2" y="-0.2" width="1.4" height="1.5">
      <feDropShadow dx="0" dy="1.6" stdDeviation="1.4" flood-color="#000" flood-opacity="0.55"/></filter>
    <filter id="f-inset" x="-0.05" y="-0.05" width="1.1" height="1.1">
      <feOffset dy="2"/><feGaussianBlur stdDeviation="2" result="b"/>
      <feComposite in="SourceGraphic" in2="b" operator="arithmetic" k2="-1" k3="1" result="i"/>
      <feFlood flood-color="#000" flood-opacity="0.6"/><feComposite in2="i" operator="in"/>
      <feComposite in2="SourceGraphic" operator="over"/></filter>`;
}

function label(parent, x, y, text, size, cls = 'pl') {
  const t = el('text', { x, y, class: cls, 'font-size': size, 'text-anchor': 'middle', 'dominant-baseline': 'central' }, parent);
  t.textContent = text;
  return t;
}

export function createPanel(container, { send }) {
  container.classList.add('fm1');
  const svg = el('svg', { viewBox: `0 0 ${PANEL.w} ${PANEL.h}`, class: 'fm1-svg', role: 'application',
    'aria-label': 'M-VAVE FM-1 front panel running SLOOP' });
  svg.style.fontFamily = LABEL_FONT;
  container.appendChild(svg);
  defs(svg);

  // ---- body
  el('rect', { x: 0, y: 0, width: PANEL.w, height: PANEL.h, rx: PANEL.r, fill: 'url(#g-body)' }, svg);
  el('rect', { x: 0, y: 0, width: PANEL.w, height: PANEL.h, rx: PANEL.r, fill: 'url(#g-sheen)' }, svg);
  el('rect', { x: 1, y: 1, width: PANEL.w - 2, height: PANEL.h - 2, rx: PANEL.r - 1, fill: 'none', stroke: '#2a2a2c', 'stroke-width': 2 }, svg);
  el('rect', { x: 3, y: 3, width: PANEL.w - 6, height: PANEL.h - 6, rx: PANEL.r - 3, fill: 'none', stroke: '#6e6f71', 'stroke-opacity': 0.35, 'stroke-width': 1 }, svg);
  for (const t of TRAYS)
    el('rect', { x: t.x, y: t.y, width: t.w, height: t.h, rx: t.r, fill: 'url(#g-tray)', stroke: '#1c1c1e', 'stroke-width': 1.2, filter: 'url(#f-inset)' }, svg);
  el('circle', { cx: FN_DOT.x, cy: FN_DOT.y, r: FN_DOT.r, fill: '#151517' }, svg);
  for (const d of KEYBED_DOTS) el('circle', { cx: d.x, cy: d.y, r: d.r, fill: '#141416' }, svg);

  // ---- screen: bezel in the SVG, SLOOP's 240 x 240 screen in a canvas over the LCD
  const b = SCREEN.bezel, l = SCREEN.lcd;
  el('rect', { x: b.x, y: b.y, width: b.w, height: b.h, rx: b.r, fill: '#0a0b0d', stroke: '#050506', 'stroke-width': 1.5, filter: 'url(#f-shadow)' }, svg);
  el('rect', { x: l.x - 1, y: l.y - 1, width: l.w + 2, height: l.h + 2, rx: l.r, fill: '#000' }, svg);
  const canvas = document.createElement('canvas');
  canvas.width = 240;
  canvas.height = 240;
  canvas.className = 'fm1-lcd';
  canvas.setAttribute('aria-label', 'SLOOP screen');
  Object.assign(canvas.style, {
    left: `${(l.x / PANEL.w) * 100}%`, top: `${(l.y / PANEL.h) * 100}%`,
    width: `${(l.w / PANEL.w) * 100}%`, height: `${(l.h / PANEL.h) * 100}%`,
  });
  container.appendChild(canvas);
  const sharpness = () => {
    const px = canvas.getBoundingClientRect().width * (window.devicePixelRatio || 1);
    canvas.classList.toggle('crisp', px >= 239.5);
  };
  if (typeof ResizeObserver !== 'undefined') new ResizeObserver(sharpness).observe(canvas);
  window.addEventListener('resize', sharpness);
  sharpness();
  const ctx = canvas.getContext('2d');
  const img = ctx.createImageData(240, 240);
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, 240, 240);

  const controls = new Map();       // id -> { g, down(), up(), setLed() }
  const pressed = new Set();        // ids currently held (pointer, keyboard or latch)
  const latched = new Set();

  function emitButton(id, down) { send({ v: 1, t: 'btn', id, down }); }
  function emitKey(k, down) { send({ v: 1, t: 'key', k, down }); }

  function setPressedVisual(id, on) {
    const c = controls.get(id);
    if (c) c.g.classList.toggle('down', on);
  }
  function press(id, sourceKey) {
    const c = controls.get(id);
    if (!c) return;
    if (!c.sources) c.sources = new Set();
    const was = c.sources.size > 0;
    c.sources.add(sourceKey);
    if (!was) {
      setPressedVisual(id, true);
      pressed.add(id);
      c.emit(true);
    }
  }
  function release(id, sourceKey) {
    const c = controls.get(id);
    if (!c || !c.sources || !c.sources.has(sourceKey)) return;
    c.sources.delete(sourceKey);
    if (c.sources.size === 0) {
      setPressedVisual(id, false);
      pressed.delete(id);
      c.emit(false);
    }
  }
  function toggleLatch(id) {
    const c = controls.get(id);
    if (!c) return;
    if (latched.has(id)) {
      latched.delete(id);
      c.g.classList.remove('latched');
      release(id, 'latch');
    } else {
      latched.add(id);
      c.g.classList.add('latched');
      press(id, 'latch');
    }
  }
  function releaseAllLatches() { for (const id of [...latched]) toggleLatch(id); }

  // pointer handling shared by buttons and keys: each pointer is its own finger
  function bindPress(g, id) {
    g.addEventListener('pointerdown', (e) => {
      if (e.button === 2) return;
      e.preventDefault();
      if (document.activeElement === g) g.blur();     // (a finger leaves no focus ring; the keyboard does)
      g.setPointerCapture(e.pointerId);
      press(id, `p${e.pointerId}`);
    });
    const up = (e) => release(id, `p${e.pointerId}`);
    g.addEventListener('pointerup', up);
    g.addEventListener('pointercancel', up);
    g.addEventListener('lostpointercapture', up);
    g.addEventListener('contextmenu', (e) => { e.preventDefault(); toggleLatch(id); });
    g.addEventListener('keydown', (e) => {
      if ((e.key === ' ' || e.key === 'Enter') && !e.repeat) { e.preventDefault(); e.stopPropagation(); press(id, 'focus'); }
    });
    g.addEventListener('keyup', (e) => {
      if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); e.stopPropagation(); release(id, 'focus'); }
    });
  }

  // ---- buttons
  for (const btn of BUTTONS) {
    const g = el('g', { class: 'ctl btn', tabindex: 0, role: 'button', 'data-id': btn.id,
      'aria-label': btn.print === 'SEL' ? 'SEL (SLOOP: SCL, scale)' : btn.print.replace('|', '/') }, svg);
    const x0 = btn.x - btn.w / 2, y0 = btn.y - btn.h / 2;
    el('title', {}, g).textContent = btn.print === 'SEL' ? 'SEL — SLOOP: SCL (key and chords)' : btn.print.replace('|', ' / ');
    const glow = el('rect', { x: x0 - 3, y: y0 - 3, width: btn.w + 6, height: btn.h + 6, rx: btn.r + 3, class: 'btn-glow', filter: 'url(#f-btnled)' }, g);
    const face = el('g', { class: 'face' }, g);
    el('rect', { x: x0, y: y0, width: btn.w, height: btn.h, rx: btn.r, fill: 'url(#g-btn)', stroke: '#0e0e10', 'stroke-width': 1.2, filter: 'url(#f-shadow)' }, face);
    el('rect', { x: x0 + 1.5, y: y0 + 1.2, width: btn.w - 3, height: btn.h * 0.42, rx: btn.r - 1.5, fill: '#fff', 'fill-opacity': 0.035 }, face);
    const fs = btn.w < 58 ? 11.5 : 10.5;
    if (btn.print.includes('|')) {
      const [a, c] = btn.print.split('|');
      label(face, btn.x, btn.y - 7.5, a, fs, 'bl');
      el('line', { x1: btn.x - 13, x2: btn.x + 13, y1: btn.y, y2: btn.y, class: 'bl-rule' }, face);
      label(face, btn.x, btn.y + 7.5, c, fs, 'bl');
    } else {
      label(face, btn.x, btn.y + 0.5, btn.print, fs, 'bl');
    }
    el('circle', { cx: x0 + btn.w - 6, cy: y0 + 6, r: 2.2, class: 'latch-mark' }, g);
    controls.set(btn.id, { g, glow, emit: (down) => emitButton(btn.id, down) });
    bindPress(g, btn.id);
  }

  // ---- keys
  for (const key of KEYS) {
    const id = `key${key.k}`;
    const g = el('g', { class: `ctl key ${key.white ? 'white' : 'black'}`, tabindex: 0, role: 'button', 'data-k': key.k,
      'aria-label': `key ${noteName(key.midi)}${key.print ? ` (${key.print})` : ''}` }, svg);
    el('title', {}, g).textContent = noteName(key.midi) + (key.print ? ` — ${key.print}` : '');
    const x0 = key.x - KEY_W / 2, y0 = key.y - KEY_H / 2;
    const face = el('g', { class: 'face' }, g);
    el('rect', { x: x0, y: y0, width: KEY_W, height: KEY_H, rx: KEY_W / 2, fill: 'url(#g-key)', stroke: '#0d0d0f', 'stroke-width': 1.2, filter: 'url(#f-shadow)' }, face);
    el('rect', { x: x0 + 2, y: y0 + 2, width: KEY_W - 4, height: KEY_H * 0.45, rx: KEY_W / 2 - 2, fill: '#fff', 'fill-opacity': 0.03 }, face);
    // the light line (the LED's diffuser): long on white keys and the last black key, short on the others
    const long = key.white || !key.print;
    const lh = long ? 49 : 26.5, lw = 4.6;
    const ly = key.white ? key.y : (long ? key.y : key.y - 11);
    const ledGlow = el('rect', { x: key.x - lw, y: ly - lh / 2 - 2, width: lw * 2, height: lh + 4, rx: lw, class: 'led-glow', filter: 'url(#f-led)' }, face);
    el('rect', { x: key.x - lw / 2, y: ly - lh / 2, width: lw, height: lh, rx: lw / 2, class: 'led-line' }, face);
    if (key.print) label(face, key.x, key.y + 17.5, key.print, 8.6, 'kl');
    el('circle', { cx: key.x, cy: key.y - KEY_H / 2 + 7, r: 2.2, class: 'latch-mark' }, g);
    controls.set(id, { g, glow: ledGlow, emit: (down) => emitKey(key.k, down) });
    bindPress(g, id);
  }

  // ---- knobs
  const potState = new Map();
  for (const kn of KNOBS) {
    const g = el('g', { class: `ctl knob ${kn.kind}`, tabindex: 0, role: kn.kind === 'pot' ? 'slider' : 'spinbutton',
      'data-id': kn.id, 'aria-label': `${kn.label}${kn.kind === 'enc' ? ' encoder' : ''}` }, svg);
    el('title', {}, g).textContent = `${kn.label} — drag up / down or use the wheel`;
    label(svg, kn.x, kn.y + KNOB_LABEL_DY, kn.label, 12.2, 'pl');
    const r = kn.d / 2;
    el('circle', { cx: kn.x, cy: kn.y, r: r + 2.5, fill: '#1b1b1d', 'fill-opacity': 0.55 }, g);   // the shadow ring
    el('circle', { cx: kn.x, cy: kn.y, r, fill: 'url(#g-knob)', stroke: '#0c0c0e', 'stroke-width': 1.2, filter: 'url(#f-shadow)' }, g);
    const rot = el('g', { class: 'rot' }, g);
    // knurling: fine ribs around the skirt, turning with the knob
    for (let i = 0; i < 36; i++) {
      const a = (i / 36) * Math.PI * 2;
      el('line', { x1: kn.x + Math.cos(a) * (r - 4.2), y1: kn.y + Math.sin(a) * (r - 4.2),
        x2: kn.x + Math.cos(a) * (r - 0.8), y2: kn.y + Math.sin(a) * (r - 0.8), class: 'rib' }, rot);
    }
    el('circle', { cx: kn.x, cy: kn.y, r: r - 5, fill: 'url(#g-knob-top)', stroke: '#111113', 'stroke-width': 0.8 }, g);
    let dot = null;
    if (kn.kind === 'pot') dot = el('circle', { cx: kn.x, cy: kn.y - (r - 9), r: 2.6, fill: '#e9e9e9' }, el('g', { class: 'pot-dot' }, g));
    const st = { angle: 0, rot, dot: dot && dot.parentNode, kn };
    potState.set(kn.id, st);
    const turnBy = (steps) => {
      st.angle += steps * 15;
      rot.setAttribute('transform', `rotate(${st.angle} ${kn.x} ${kn.y})`);
    };
    if (kn.kind === 'enc') {
      const drag = new EncoderDrag(10);
      const emit = (steps) => { if (steps) { send({ v: 1, t: 'enc', id: kn.id, d: steps }); turnBy(steps); } };
      g.addEventListener('pointerdown', (e) => { e.preventDefault(); g.setPointerCapture(e.pointerId); drag.start(e.clientX, e.clientY); g.classList.add('turning'); });
      g.addEventListener('pointermove', (e) => { if (g.hasPointerCapture(e.pointerId)) emit(drag.move(e.clientX, e.clientY)); });
      const end = () => { drag.end(); g.classList.remove('turning'); };
      g.addEventListener('pointerup', end);
      g.addEventListener('pointercancel', end);
      g.addEventListener('wheel', (e) => { e.preventDefault(); emit(drag.wheel(e.deltaY, e.deltaMode)); }, { passive: false });
      g.addEventListener('keydown', (e) => {
        const s = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1, PageUp: 5, PageDown: -5 }[e.key];
        if (s) { e.preventDefault(); e.stopPropagation(); emit(s); }
      });
    } else {
      const pot = new PotDrag(724, 1.6);
      const show = (v) => {
        const a = -150 + (v / 1023) * 300;
        st.dot.setAttribute('transform', `rotate(${a} ${kn.x} ${kn.y})`);
        rot.setAttribute('transform', `rotate(${a} ${kn.x} ${kn.y})`);
        g.setAttribute('aria-valuenow', String(v));
      };
      g.setAttribute('aria-valuemin', '0');
      g.setAttribute('aria-valuemax', '1023');
      const emit = (v) => { if (v !== null) { send({ v: 1, t: 'pot', id: 'MASTER', val: v }); show(v); } };
      show(pot.value);
      st.setPot = (v) => { pot.value = v; show(v); };
      g.addEventListener('pointerdown', (e) => { e.preventDefault(); g.setPointerCapture(e.pointerId); pot.start(e.clientY); g.classList.add('turning'); });
      g.addEventListener('pointermove', (e) => { if (g.hasPointerCapture(e.pointerId)) emit(pot.move(e.clientY)); });
      const end = () => g.classList.remove('turning');
      g.addEventListener('pointerup', end);
      g.addEventListener('pointercancel', end);
      g.addEventListener('wheel', (e) => { e.preventDefault(); emit(pot.wheel(e.deltaY)); }, { passive: false });
      g.addEventListener('keydown', (e) => {
        const s = { ArrowUp: 16, ArrowRight: 16, ArrowDown: -16, ArrowLeft: -16, PageUp: 128, PageDown: -128 }[e.key];
        if (s) { e.preventDefault(); e.stopPropagation(); emit(pot.set(pot.value + s)); }
      });
    }
  }

  // ---- computer keyboard: FM-1 keys and a few buttons while the page has focus
  const kbdDown = new Map();
  window.addEventListener('keydown', (e) => {
    if (e.ctrlKey || e.metaKey || e.altKey || e.repeat) return;
    if (e.target && e.target.closest && e.target.closest('input, textarea, select')) return;
    if (e.code === 'Escape') { releaseAllLatches(); return; }
    const k = KEYMAP[e.code], b = BUTTON_KEYMAP[e.code];
    const id = k !== undefined ? `key${k}` : b;
    if (!id) return;
    e.preventDefault();
    kbdDown.set(e.code, id);
    press(id, `kbd${e.code}`);
  });
  window.addEventListener('keyup', (e) => {
    const id = kbdDown.get(e.code);
    if (!id) return;
    kbdDown.delete(e.code);
    release(id, `kbd${e.code}`);
  });
  window.addEventListener('blur', () => { for (const [code, id] of kbdDown) release(id, `kbd${code}`); kbdDown.clear(); });

  // ---- state from SLOOP
  function setLeds(s) {
    for (let i = 0; i < s.length; i++) {
      const id = i < LED_BUTTON_ORDER.length ? LED_BUTTON_ORDER[i] : `key${i - LED_BUTTON_ORDER.length}`;
      const c = controls.get(id);
      if (!c) continue;
      const v = s.charCodeAt(i) - 48;
      c.g.classList.toggle('lit', v === 2);
      c.g.classList.toggle('dim', v === 1);
    }
  }

  function drawRect(x, y, w, h, px) {
    const d = img.data;
    for (let row = 0; row < h; row++) {
      let o = ((y + row) * 240 + x) * 4, p = row * w * 2;
      for (let col = 0; col < w; col++, p += 2, o += 4) {
        const v = (px[p] << 8) | px[p + 1];
        d[o] = ((v >> 11) * 527 + 23) >> 6;
        d[o + 1] = (((v >> 5) & 63) * 259 + 33) >> 6;
        d[o + 2] = ((v & 31) * 527 + 23) >> 6;
        d[o + 3] = 255;
      }
    }
    ctx.putImageData(img, 0, 0, x, y, w, h);
  }

  function reset() {
    for (const id of [...pressed]) {
      const c = controls.get(id);
      if (c && c.sources) c.sources.clear();
      setPressedVisual(id, false);
    }
    pressed.clear();
    latched.clear();
    for (const c of controls.values()) c.g.classList.remove('latched', 'lit', 'dim');
  }

  return { setLeds, drawRect, reset, releaseAllLatches, svg, canvas,
    setMaster: (v) => potState.get('MASTER').setPot(v) };
}
