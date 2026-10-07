# Web panel protocol (v1)

The browser panel and a SLOOP target (the host app `sloop_host`, or the ESP32 firmware) talk over
one WebSocket at **`/ws`** on the same HTTP server that serves the panel. Implementation:
`components/sloop_proto` (shared by both targets), tests: `tests/host/test_proto.c`,
`tests/web/test_ws_e2e.py`.

- Control and state messages are **JSON text frames**: flat objects, `"v": 1` (protocol version)
  and `"t"` (type) in every message. Unknown keys are ignored; a message with a missing or other
  `v` is refused.
- Screen updates are **binary frames** (dense pixel data).
- The device never interprets the panel's look: controls are named by their printed FM-1 label,
  and SLOOP itself decides what a press, a hold, a combination or a turn means.

## Browser -> device

| `t` | Fields | Meaning |
| --- | --- | --- |
| `hello` | (`client`: free text), (`screen`: false), (`midi`: true) | (Re)start the session: the device answers `hello`, the whole screen, `leds`, `status`. `screen: false`: none of those (an editor); `midi: true`: also send SLOOP's MIDI out |
| `btn` | `id`, `down` (bool) | A button goes down / up. `id`: `FX SCL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+` |
| `key` | `k` (0..26), `down` | A key of the keyboard, `0` = F3 .. `26` = G5 (16 white, 11 black). |
| `enc` | `id`, `d` (-64..64, not 0) | Detent steps of an encoder, `+` = clockwise. `id`: `SELECT ALGORITHM PRESETS K1 K2 K3 K4` |
| `pot` | `id` = `MASTER`, `val` (0..1023) | The MASTER volume pot (an analog pot on the FM-1). |
| `midi` | `data`: `[status, d1(, d2)]` | A MIDI channel message (`0x80..0xEF`), as if from USB MIDI. |
| `ping` | `n` (integer) | Answered with `pong` and the same `n` (latency measurement). |
| `full` | | Send the whole screen again. |
| `sysex` | `data`: hex string `"F0...F7"` | A SysEx message for SLOOP: the web editor protocol (upstream `web/EDITOR_PROTOCOL.md`, `F0 7D 46 4C cmd .. F7`, at most 640 bytes between F0 and F7). The client then receives SLOOP's SysEx replies and pushes. |

Examples:

```json
{"v":1,"t":"btn","id":"PLAY","down":true}
{"v":1,"t":"btn","id":"PLAY","down":false}
{"v":1,"t":"key","k":12,"down":true}
{"v":1,"t":"enc","id":"K1","d":-1}
{"v":1,"t":"pot","id":"MASTER","val":800}
{"v":1,"t":"midi","data":[144,60,100]}
```

Timing semantics (enforced on the device, `sloop_runtime.c`): events are applied in order; a
button or key is held down at least 20 ms (the FM-1's debounce guarantees the same; SLOOP reads
key levels in the audio interrupt) and a control changes at most once per UI pass. Long presses,
holds, layers and combinations are measured by SLOOP from the down / up times, so the panel only
has to send the edges when they happen. Encoders have no push switch on the FM-1.

Encoder detents reach SLOOP one per UI pass (~1 ms), as from a hand on a physical encoder: some
controls take one step per read (ALGORITHM moves one track per read) and SLOOP accelerates turns
whose steps come less than 60 ms apart (x3 or x6 on wide ranges such as the tempo). So `d: 1`
messages sent as the knob moves behave exactly like the hardware; one message with `d: 5` is a
fast flick and is accelerated, as it would be on the device.

## Device -> browser

| `t` | Fields |
| --- | --- |
| `hello` | `proto`, `fw`, `sloop` (version), `upstream` (commit), `target` (`host`, `esp32s3`, `esp32s3-qemu`), `lcd` `[240,240]`, `keys` 27, `buttons`, `encoders`, `pots` (the ids above, in order) |
| `leds` | `s`: 41 characters, one per control, `0` off, `1` dim, `2` lit: the 14 buttons in `buttons` order, then the 27 keys |
| `status` | `playing`, `rec`, `track` (0..3), `page` (-1 = TRACKS/home), `menu` (the HOME menu is open), `bpm`, `cpu` (audio render load, %), `uptime` (ms) |
| `pong` | `n` |
| `err` | `msg`: why a message was refused (the session continues) |
| `sysex` | `data`: hex `"F0...F7"`: SLOOP's editor replies and pushes (to clients that sent SysEx) |
| `midi` | `data`: `[status, d1(, d2)]`: notes SLOOP sends out (to clients that asked with `hello` `midi: true`) |

## SLOOP's web editor

The device also serves upstream's editor at **`/editor.html`** (unmodified, with one `<script>` line
added to its `<head>`: `web/editor-bridge.js`). The bridge gives the editor a Web MIDI-like port
named "Felucca" whose SysEx goes over this WebSocket (`hello` with `screen: false`, then `sysex`),
so the whole editor (every parameter, the drum grid, the mixer, user presets, sample upload) works
against the ESP32 or the host app, in any browser.

`leds` and `status` are sent on connect and whenever they change.

### Binary: screen rectangle

```
offset  size  field
0       1     0x01  message type: display rectangle
1       1     0x00  pixel format: RGB565, big-endian (as the ST7789 receives it)
2       2     x     (little-endian)
4       2     y
6       2     w
8       2     h
10      w*h*2 pixels, row by row
```

After `hello` the device sends the whole screen (0, 0, 240, 240), then one rectangle per
update (~30 per second at most) bounding everything SLOOP drew since the last one. A client that
cannot keep up skips updates and gets the whole screen again when it can.

The screen is what SLOOP drew through its own `lcd.c` into a virtual ST7789: the browser shows the
firmware's screen, it does not render menus itself.

## HTTP endpoints (same server)

| Path | |
| --- | --- |
| `/` , `/panel.js`, ... | the panel (gzip on the ESP32) |
| `/editor.html`, `/editor-bridge.js`, `/fukiai.ttf` | SLOOP's web editor |
| `/device.html` | status, network settings, firmware update page |
| `GET /api/status` | JSON diagnostics: `boot` (boots of this flash: changes on a restart), firmware (`app`: version, slot, OTA state), audio frames / underruns / render max / load, heap, stacks, store, panels, USB-MIDI |
| `GET`, `POST /api/settings`, `POST /api/settings/reset` | network settings (ESP32 only; docs/CONFIGURATION.md) |
| `GET /api/store` | SLOOP's store (448 KiB: settings, projects, autosave, user presets, user samples) as `sloop-store-<hostname>.bin` (ESP32 only) |
| `POST /api/store` | restore: body = such a file, header `X-Sloopy-Password`; checked with SLOOP's rules, staged, applied at the restart: `200 {"ok":true,"restart":true,"projects":…}`, `403`, `400` (ESP32 only) |
| `POST /api/ota` | firmware update: body = the application image, header `X-Sloopy-Password` = the current Wi-Fi password (URI-encoded); `200 {"ok":true,"restart":true,"partition":"ota_1"}`, `403`, `400 {"error":…}` (ESP32 only; docs/CONFIGURATION.md) |

## Versioning

`v` changes only for incompatible changes. New message types or fields may be added within v1;
clients ignore what they do not know.
