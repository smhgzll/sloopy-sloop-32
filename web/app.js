// SPDX-License-Identifier: GPL-3.0-only
// sloopy-sloop-32: the panel page: the FM-1 panel wired to a SLOOP target over the WebSocket.
import { createPanel } from './panel.js';
import { Session, wsUrl, midiToMsg } from './proto.js';

const conn = document.getElementById('conn');
const info = document.getElementById('info');
let hello = null, status = null, rtt = null;

function showInfo() {
  const parts = [];
  if (hello) parts.push(`${hello.sloop} · ${hello.target}`);
  if (status) parts.push(`${status.playing ? '▶' : '■'} ${status.bpm} bpm · track ${status.track + 1} · cpu ${status.cpu}%`);
  if (rtt !== null) parts.push(`${rtt} ms`);
  info.textContent = parts.join('   ');
}

let session;
const panel = createPanel(document.getElementById('panel'), { send: (m) => session && session.send(m) });
session = new Session(wsUrl(location), {
  onState: (s) => {
    conn.className = `conn ${s}`;
    conn.textContent = s === 'open' ? 'connected' : s === 'connecting' ? 'connecting…' : 'disconnected';
    if (s !== 'open') panel.reset();
  },
  onHello: (h) => { hello = h; showInfo(); },
  onStatus: (s) => { status = s; showInfo(); },
  onLeds: (s) => panel.setLeds(s),
  onRect: (r) => panel.drawRect(r.x, r.y, r.w, r.h, r.px),
  onRtt: (ms) => { rtt = ms; showInfo(); },
  onError: (msg) => console.warn('SLOOP:', msg),
});
session.connect();

const helpBtn = document.getElementById('help-btn'), help = document.getElementById('help');
helpBtn.addEventListener('click', () => {
  help.hidden = !help.hidden;
  helpBtn.setAttribute('aria-expanded', String(!help.hidden));
});
// ---- MIDI in: a keyboard on this computer / phone plays SLOOP (its channels pick the tracks)
const midiBtn = document.getElementById('midi-btn');
let midiAccess = null, midiCount = 0;
function bindMidiInputs() {
  if (!midiAccess) return;
  let n = 0;
  for (const input of midiAccess.inputs.values()) {
    input.onmidimessage = midiBtn.getAttribute('aria-pressed') === 'true'
      ? (e) => { const m = midiToMsg(e.data); if (m) { session.send(m); midiCount++; } } : null;
    n++;
  }
  midiBtn.textContent = midiBtn.getAttribute('aria-pressed') === 'true' ? `midi in (${n})` : 'midi in';
}
async function setMidi(on) {
  if (on && !navigator.requestMIDIAccess) {
    // (browsers give Web MIDI to https:// and localhost pages only; the board serves http://)
    midiBtn.textContent = window.isSecureContext ? 'no Web MIDI here' : 'midi: needs localhost / https';
    return;
  }
  if (on && !midiAccess) {
    try { midiAccess = await navigator.requestMIDIAccess(); } catch { midiBtn.textContent = 'MIDI refused'; return; }
    midiAccess.onstatechange = bindMidiInputs;
  }
  midiBtn.setAttribute('aria-pressed', String(on));
  bindMidiInputs();
  try { localStorage.setItem('sloopy.midiIn', on ? '1' : '0'); } catch { /* (no storage: fine) */ }
}
midiBtn.addEventListener('click', () => setMidi(midiBtn.getAttribute('aria-pressed') !== 'true'));
try { if (localStorage.getItem('sloopy.midiIn') === '1') setMidi(true); } catch { /* (no storage) */ }

window.sloopPanel = { panel, session, midiCount: () => midiCount };   // (debugging, tests)
