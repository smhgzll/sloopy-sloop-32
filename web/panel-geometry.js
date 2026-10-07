// SPDX-License-Identifier: GPL-3.0-only
// sloopy-sloop-32: the M-VAVE FM-1 front panel as data.
//
// One normalized coordinate system: the panel is 1000 units wide and keeps the device's aspect
// ratio (1.676 : 1, measured on a straight-on product photo, see docs/UI_SPEC.md). Every position
// below was measured on that photo (body 486 x 290 px -> 1000 x 597 units) and rounded; the
// controls are wired to SLOOP by the ids of docs/PROTOCOL.md. Pure data: no DOM, testable in node.

export const PANEL = { w: 1000, h: 597, r: 31 };

// the screen: a black rounded bezel around SLOOP's 240 x 240 LCD
export const SCREEN = {
  bezel: { x: 257, y: 43, w: 237, h: 237, r: 30 },
  lcd: { x: 290.5, y: 76.5, w: 170, h: 170, r: 3 },
};

// rotary controls: endless encoders (detents) and the MASTER pot (analog, 0..1023)
export const KNOBS = [
  { id: 'MASTER', kind: 'pot', label: 'MASTER', x: 83, y: 93, d: 48 },
  { id: 'SELECT', kind: 'enc', label: 'SELECT', x: 190, y: 93, d: 48 },
  { id: 'PRESETS', kind: 'enc', label: 'PRESETS', x: 83, y: 198, d: 48 },
  { id: 'ALGORITHM', kind: 'enc', label: 'ALGORITHM', x: 190, y: 198, d: 48 },
  { id: 'K1', kind: 'enc', label: 'KNOB1', x: 563, y: 93, d: 46 },
  { id: 'K2', kind: 'enc', label: 'KNOB2', x: 676, y: 93, d: 46 },
  { id: 'K3', kind: 'enc', label: 'KNOB3', x: 789, y: 93, d: 46 },
  { id: 'K4', kind: 'enc', label: 'KNOB4', x: 902, y: 93, d: 46 },
];
export const KNOB_LABEL_DY = -46;          // label centre above the knob centre

// recessed trays
export const TRAYS = [
  { id: 'fn', x: 534, y: 163, w: 399, h: 140, r: 18 },
  { id: 'oct', x: 56, y: 255, w: 156, h: 47, r: 11 },
  { id: 'keys', x: 33, y: 342, w: 934, h: 216, r: 28 },
];

// the 12 function buttons (2 x 6 in the tray) and OCT- / OCT+.
// print: what the FM-1 prints on the button; id: SLOOP's name (the protocol id). The FM-1 prints
// "SEL" on the button SLOOP uses as SCL (scale).
const FN = [
  ['FX', 'FX'], ['SCL', 'SEL'], ['ENV', 'ENV'], ['LFO', 'LFO'], ['EDIT', 'EDIT'], ['GLO', 'GLO'],
  ['HOME', 'HOME'], ['SAVE', 'SAVE'], ['ARP', 'ARP'], ['SEQ', 'SEQ'], ['PLAY', 'PLAY|STOP'], ['REC', 'REC'],
];
export const BUTTONS = [
  ...FN.map(([id, print], i) => ({
    id, print, x: 576 + (i % 6) * 63, y: 201.5 + Math.floor(i / 6) * 63, w: 55.5, h: 55.5, r: 9,
  })),
  { id: 'OCT-', print: 'OCT-', x: 96, y: 278.5, w: 60, h: 36, r: 7 },
  { id: 'OCT+', print: 'OCT+', x: 173, y: 278.5, w: 60, h: 36, r: 7 },
];
export const FN_DOT = { x: 733.5, y: 233, r: 1.6 };   // the small mark between the button rows

// the keyboard: 27 keys F3..G5, 16 white below, 11 black above (pills with a light line, the LED)
const WHITE_X0 = 74.1, WHITE_PITCH = 56.65;
const BLACK_PRINT = ['OP1', 'OP2', 'OP3', 'OP4', 'OP5', 'OP6', 'PIT', 'GLO', 'MONO', 'POLY', ''];
const SEMIS = [0, 2, 4, 5, 7, 9, 11];          // white keys of an octave from C
export const KEY_W = 48, KEY_H = 90;
export const WHITE_Y = 496, BLACK_Y = 400;

function buildKeys() {
  const keys = [];
  let white = 0, black = 0;
  for (let k = 0; k < 27; k++) {
    const midi = 53 + k;                         // F3 = MIDI 53
    const isWhite = SEMIS.includes(midi % 12);
    if (isWhite) {
      keys.push({ k, midi, white: true, x: WHITE_X0 + white * WHITE_PITCH, y: WHITE_Y, print: '', index: white });
      white++;
    } else {
      // between the white key before and the one after
      const x = WHITE_X0 + (white - 0.5) * WHITE_PITCH;
      keys.push({ k, midi, white: false, x, y: BLACK_Y, print: BLACK_PRINT[black], index: black });
      black++;
    }
  }
  return keys;
}
export const KEYS = buildKeys();

// marks on the keybed between white keys with no black key between them (B-C, E-F)
export const KEYBED_DOTS = KEYS.filter((k, i) => k.white && i + 1 < KEYS.length && KEYS[i + 1].white)
  .map((k) => ({ x: k.x + WHITE_PITCH / 2, y: 450, r: 1.6 }));

export const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
export function noteName(midi) { return NOTE_NAMES[midi % 12] + (Math.floor(midi / 12) - 1); }

// LED order of the protocol's "leds" string: the 14 buttons (SLOOP's order), then the 27 keys
export const LED_BUTTON_ORDER = ['FX', 'SCL', 'ENV', 'LFO', 'EDIT', 'GLO', 'HOME', 'SAVE', 'ARP', 'SEQ', 'PLAY', 'REC', 'OCT-', 'OCT+'];

// computer keyboard shortcuts (KeyboardEvent.code): keys of the FM-1 and a few buttons
export const KEYMAP = {
  // white F3..A4 on the bottom letter row, their black keys on the row above
  KeyZ: 0, KeyS: 1, KeyX: 2, KeyD: 3, KeyC: 4, KeyF: 5, KeyV: 6, KeyB: 7, KeyH: 8, KeyN: 9, KeyJ: 10,
  KeyM: 11, Comma: 12, KeyL: 13, Period: 14, Semicolon: 15, Slash: 16, Quote: 17,
  // B4..G5 on the top letter row, their black keys on the number row
  KeyQ: 18, KeyW: 19, Digit3: 20, KeyE: 21, Digit4: 22, KeyR: 23, KeyT: 24, Digit6: 25, KeyY: 26,
};
export const BUTTON_KEYMAP = { Space: 'PLAY', BracketLeft: 'OCT-', BracketRight: 'OCT+' };
