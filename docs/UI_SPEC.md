# Browser panel (the virtual FM-1)

Files: `web/index.html`, `web/style.css`, `web/app.js`, `web/panel.js` (SVG components),
`web/panel-geometry.js` (the panel as data), `web/proto.js` (protocol client). Tests:
`tests/web/test_panel.mjs`, `tests/web/test_ws_e2e.py`.

## Principle

Opening the page should feel like the FM-1 running SLOOP. The panel is vector components on one
normalized coordinate system, not a photo with hotspots. The screen is SLOOP's own framebuffer;
the panel never re-implements SLOOP's screens or input logic.

## Geometry

The panel is 1000 x 597 units (aspect 1.676). Every control was measured on a straight-on product
photo (`assets/references/mwave-fm1-front-source.txt`: 576 x 576 px, body 486 x 290 px), then
checked by overlaying the rendering on the photo (edge overlay: knobs, labels, trays, buttons,
bezel and all 27 keys coincide within a few units). The photo is third-party and is not committed.

| Element | Units (x, y: centre unless noted) |
| --- | --- |
| body | 1000 x 597, corner radius 31 |
| MASTER (pot), SELECT | (83, 93), (190, 93), d 48; labels 46 above |
| PRESETS, ALGORITHM | (83, 198), (190, 198) |
| KNOB1..4 | x 563, 676, 789, 902; y 93; d 46 |
| screen bezel / LCD | bezel x 257 y 43 237 x 237 r 30; LCD x 290.5 y 76.5 170 x 170 |
| function tray | x 534 y 163 399 x 140 r 18; buttons 55.5 square, pitch 63, rows y 201.5 / 264.5, first x 576 |
| OCT tray | x 56 y 255 156 x 47; OCT- (96, 278.5), OCT+ (173, 278.5), 60 x 36 |
| keybed | x 33 y 342 934 x 216 r 28 |
| white keys | 16, x 74.1 + 56.65 n, y 496; pills 48 x 90 with a 49-unit light line |
| black keys | 11, centred between their white neighbours, y 400; short light line + printing |

Printing as on the hardware: knob labels `MASTER SELECT PRESETS ALGORITHM KNOB1..KNOB4`, buttons
`FX SEL ENV LFO EDIT GLO / HOME SAVE ARP SEQ PLAY|STOP REC`, `OCT- OCT+`, black keys
`OP1 OP2 OP3 OP4 OP5 OP6 PIT GLO MONO POLY` (the 11th blank). The 2nd button prints **SEL**;
SLOOP calls it **SCL** (its tooltip and accessible name say so). Small marks sit on the keybed
between white keys that have no black key between them (B–C, E–F), and between the button rows.

To re-measure against your own photo: photograph the device straight on, crop to the body,
scale to 1000 units wide, and adjust `web/panel-geometry.js`; `tests/web/test_panel.mjs` checks
that nothing overlaps or leaves its tray.

## Lights

SLOOP drives an LED under each button and key (`leds` messages): off / dim / lit. Buttons glow
behind their face; keys light their line (the FM-1's light pipes), dim for SLOOP's landmarks
(keys 1, 5, 9, 13 while a layer is held), full for what is on.

## Interaction

| Input | Effect |
| --- | --- |
| pointer down / up on a button or key (mouse, touch, pen) | `btn` / `key` down / up; each finger is independent, so a layer button can be held while keys are played |
| right-click a button or key | latch it down (amber mark); right-click again or `Esc` releases all |
| drag up / right on an encoder | clockwise detents (10 px each), sent as they happen |
| wheel on an encoder | one detent per notch (trackpads: per 50 px) |
| drag / wheel on MASTER | 0..1023 |
| keyboard: `Z X C V B N M , . /` + `S D F H J L ; '` | F3..A4 and their black keys |
| keyboard: `Q W E R T Y` + `3 4 6` | B4..G5 and their black keys |
| keyboard: `Space` / `[` / `]` | PLAY, OCT-, OCT+ (held as long as the key) |
| focus + `Space` / `Enter`; arrows / PageUp / PageDown on knobs | the same as pointer / turns |

What a press means (tap, hold, layer, lock, long press, combinations) is decided by SLOOP from
the edges and their timing, exactly as with the hardware; the device enforces the hardware's
timing (docs/PROTOCOL.md). If the connection drops, the device releases whatever that panel held.

## Screen

A 240 x 240 canvas over the LCD, fed by the binary rectangles of the protocol. It is drawn with
nearest-neighbour pixels when it gets at least 240 device pixels, smoothed when smaller (shrinking
with nearest-neighbour erases SLOOP's 1-px strokes).

## Layout

The panel fills the window width while fitting its height, keeping its proportions; a status bar
below shows the connection, SLOOP's version and target, transport, tempo, track, CPU, round trip.
`?ws=ws://host:port/ws` connects the page to another target (e.g. the QEMU firmware).
