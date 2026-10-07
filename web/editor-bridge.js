// SPDX-License-Identifier: GPL-3.0-only
// sloopy-sloop-32: SLOOP's own web editor (upstream web/editor.html, served unmodified as
// /editor.html with this script added to its <head>) talks Web MIDI SysEx to an FM-1. Here there
// is no USB: this script gives the editor a Web MIDI-like port named "Felucca" whose SysEx goes
// over the panel's WebSocket (protocol message "sysex", docs/PROTOCOL.md) to SLOOP's editor.c on
// the device. Works in every browser, Web MIDI support or not.
(function () {
  'use strict';
  const q = new URLSearchParams(location.search);
  const url = q.get('ws') || `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`;
  const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, '0')).join('').toUpperCase();
  const unhex = (s) => { const u = new Uint8Array(s.length / 2); for (let i = 0; i < u.length; i++) u[i] = parseInt(s.substr(2 * i, 2), 16); return u; };

  const input = { id: 'sloopy-in', name: 'Felucca (sloopy-sloop-32)', manufacturer: 'sloopy-sloop-32', type: 'input',
    state: 'disconnected', connection: 'open', onmidimessage: null, open: () => Promise.resolve(input), close: () => Promise.resolve(input),
    addEventListener(type, fn) { if (type === 'midimessage') this.onmidimessage = fn; } };
  const pending = [];
  let ws = null, access = null;
  const output = { id: 'sloopy-out', name: 'Felucca (sloopy-sloop-32)', manufacturer: 'sloopy-sloop-32', type: 'output',
    state: 'disconnected', connection: 'open', open: () => Promise.resolve(output), close: () => Promise.resolve(output),
    send(data) {
      const u8 = data instanceof Uint8Array ? data : Uint8Array.from(data);
      const msg = JSON.stringify({ v: 1, t: 'sysex', data: hex(u8) });
      if (ws && ws.readyState === 1) ws.send(msg); else pending.push(msg);
    },
    clear() {} };

  function setState(s) {
    if (input.state === s) return;
    input.state = output.state = s;
    if (access && access.onstatechange) {
      access.onstatechange({ port: output });
      access.onstatechange({ port: input });
    }
  }

  function connect() {
    ws = new WebSocket(url);
    ws.onopen = () => {
      ws.send(JSON.stringify({ v: 1, t: 'hello', client: 'editor', screen: false }));
      setState('connected');
      for (const m of pending.splice(0)) ws.send(m);
    };
    ws.onmessage = (ev) => {
      if (typeof ev.data !== 'string') return;
      let m;
      try { m = JSON.parse(ev.data); } catch { return; }
      if (m.v === 1 && m.t === 'sysex' && typeof m.data === 'string' && input.onmidimessage)
        input.onmidimessage({ data: unhex(m.data), timeStamp: performance.now() });
      else if (m.v === 1 && m.t === 'err') console.warn('SLOOP:', m.msg);
    };
    ws.onclose = () => { setState('disconnected'); setTimeout(connect, 1000); };
  }

  access = {
    sysexEnabled: true,
    inputs: new Map([[input.id, input]]),
    outputs: new Map([[output.id, output]]),
    onstatechange: null,
  };
  connect();
  Object.defineProperty(navigator, 'requestMIDIAccess', {
    configurable: true,
    value: () => new Promise((resolve) => {
      if (input.state === 'connected') { resolve(access); return; }
      const t0 = Date.now();
      (function wait() { if (input.state === 'connected' || Date.now() - t0 > 3000) resolve(access); else setTimeout(wait, 50); })();
    }),
  });
  window.sloopyEditorBridge = { url, input, output };
})();
