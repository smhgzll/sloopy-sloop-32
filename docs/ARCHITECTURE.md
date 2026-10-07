# Architecture

```text
                 Browser: the FM-1 panel (web/)
   SVG controls, SLOOP's screen in a canvas, WebSocket /ws (docs/PROTOCOL.md)
                              |
        +---------------------+----------------------+
        | host app (host/)                           | ESP32-S3 firmware (main/, components/)
        |   ws_server.c (poll, RFC 6455)             |   web_server: esp_http_server + /ws,
        |                                            |   panel gzip-embedded; Wi-Fi / QEMU Ethernet
        +---------------------+----------------------+
                              |  components/sloop_proto: the protocol hub (shared)
                              |                     components/usb_midi: SLOOP as a USB-MIDI
                              |                     device (TinyUSB, native USB; opt-in)
                              v                              |
        sloop.h  mailboxes: input events in; screen, LEDs, status out; MIDI / SysEx per transport
        ---------------------------------------------------------------------------
        components/sloop_core: upstream SLOOP, compiled unmodified (port/sloop_unity.c)
          engines, drums, voices, FX, sequencer, arranger, UI, projects, presets, editor
        ---------------------------------------------------------------------------
        virtual FM-1 HAL (port/hal/*.h, replaces upstream firmware/hal)
          ST7789 -> framebuffer | key/encoder matrix | ALNK audio link | NOR flash | clock
        ---------------------------------------------------------------------------
        sloop_platform.h: clock, two locks, storage, reboot, log
          host/platform_host.c (Linux)   |   components/sloop_port (ESP-IDF)
                              |                              |
                     ALSA / WAV / null              audio_backend: I2S -> PCM5102A (null in QEMU)
```

## The core

`components/sloop_core/port/sloop_unity.c` includes upstream `firmware/src/*.c` in upstream's own
order (`felucca.c`) and two port files in place of the FM-1's hardware glue:

- `sloop_runtime.c` replaces `main.c`: `sloop_boot()`, `sloop_ui_step()` (the main loop body:
  MASTER pot, editor service, `ui_input`, `ui_leds`, `ui_draw`, autosave, ~15 ms frames),
  `sloop_audio_render()` (runs `audio.c`'s ISR through the virtual ALNK), and the mailboxes.
- `sloop_usb_shim.c` replaces `usb.c`: the MIDI and SysEx rings and entry points SLOOP uses.

Upstream's include of `../hal/fm1_xip.h` from `eng_sample.c` stays (it is inert: user-sample reads
go through the port's `SMP_USER_XIP` hook). Nothing in `upstream/` is edited.

## Contexts and concurrency

SLOOP was written for one core: the audio ISR preempts the main loop, `fm1_irq_off/on` protect
multi-word updates, SPSC rings with a compiler barrier carry MIDI. The port keeps that model:

| FM-1 | Port |
| --- | --- |
| audio ISR (ALNK0, prio 3) | audio context: `sloop_audio_render()`, bracketed by `sloop_plat_render_begin/end` |
| main loop | UI context: `sloop_boot()` + `sloop_ui_step()` every ~1 ms |
| TIMER5 ISR: input scan, USB poll, `fm1_ms` | done at the start of each UI step (and in `fm1_wdt_feed` waits) |
| `fm1_irq_off/on` (cli / sti) | the audio lock: a mutex also taken by each render, non-nesting, no-op inside the render |

On the ESP32-S3 both SLOOP tasks are pinned to **core 1**, audio at priority 20, UI at 5, so the UI
never observes half a render, exactly as on the FM-1. Wi-Fi, lwIP, httpd and the protocol pump
run on **core 0** and touch SLOOP only through the mailboxes (the io lock, held for copies only),
as do TinyUSB and the USB-MIDI bridge task when `SLOOPY_USB_MIDI` is on.
On Linux the threads are free-running; the mutex gives the same exclusion.

## MIDI transports

On the FM-1, MIDI and the web editor's SysEx go over USB. The port has two transports, each
attached on its own (`sloop_midi_attach(SLOOP_MIDI_WEB | SLOOP_MIDI_USB, ...)`):

- the WebSocket hub (`sloop_proto`): the panel's `midi in`, SLOOP's editor served at `/editor.html`
  (a Web MIDI-like bridge), MIDI out to clients that ask;
- the USB-MIDI device (`components/usb_midi`, `SLOOPY_USB_MIDI`): the FM-1's own USB-MIDI interface
  (the configuration descriptor is byte-identical to upstream `usb.c`'s), named "Felucca
  (sloopy-sloop-32)", so upstream's editor opened in Chrome finds it over Web MIDI as it finds an
  FM-1.

Input from both goes into SLOOP's MIDI ring and SysEx inbox. Output (editor replies and pushes,
the keys' MIDI notes) is read by every attached transport at its own position in the shim's rings;
a writer waits for the slowest one, as `usb.c` waits for a slow host. So a transport must keep
reading: the USB bridge waits for a host that stops reading for 200 ms (as `usb.c`), then drops
SLOOP's output until it reads again, and never holds up the web editor (host test
`test_usb_midi`). Every attached transport gets every reply, as every editor on the hub does.

## Input

The browser sends control edges by printed label. The runtime maps them to the FM-1's matrix ids
using upstream's `PANEL_DEFAULT` wiring (so SLOOP's own calibration and learned table behave as on
the device) and feeds the virtual matrix with hardware-like timing: a press lasts at least 20 ms
(SLOOP reads keys in the audio ISR), a control changes once per UI pass, encoder detents arrive one
per UI pass (SLOOP's acceleration and one-step-per-read controls see a real turn). Holds, layers,
long presses, combinations are SLOOP's own logic. A disconnecting panel releases what it held.

## Display

`lcd.c` talks ST7789 to the virtual controller, which writes RGB565 big-endian pixels into a
240x240 framebuffer and tracks a dirty box. Each UI step publishes the changed rectangle into a
second framebuffer under the io lock; the protocol pump sends rectangles (~30/s) and whole
screens to new clients. The browser draws them into a canvas: no screen logic in JavaScript.

## Audio

`audio.c` renders a half buffer of 256 frames (5.8 ms at 44.1 kHz) as int32 with 24-bit samples.
ESP32: the audio task renders, then `i2s_channel_write` blocks until the DMA ring (4 x 256 frames)
takes it: the I2S clock paces SLOOP. Samples go out as 32-bit left-justified slots (Philips I2S)
to the PCM5102A. `auto_clear` makes underruns and flash writes silent, as the FM-1 silences its
DMA buffer during flash erases. QEMU: the null backend paces by `esp_timer`.

SLOOP's own overload protection stays active: a render taking > 85 % of its period sheds a voice.

## Memory

| Where | What |
| --- | --- |
| internal RAM | SLOOP's tracks, voices, mixer and sequencer state, the audio half buffers (`abuf`), I2S DMA and staging buffers, task stacks |
| PSRAM (`.ext_ram.bss`) | SLOOP's `.pool` section: FX delay/chorus/reverb lines, slicer and punch rings, draw canvas, granular state, project buffers; the two port framebuffers, the per-transport SysEx buffers (553 KB). The USB-MIDI bridge state. Wi-Fi/lwIP buffers (hardware profile) |
| flash | firmware, the panel (gzip), SLOOP's samples and tables (rodata), the `sloop` store partition |

The `.pool` placement is a linker fragment (`components/sloop_core/linker.lf`). Delay lines in
PSRAM are read sequentially (cache friendly); their real cost is a hardware measurement (H-5).

## Storage

SLOOP's `storage.c` keeps A/B copies of settings, 4 projects, the autosave and 32 user presets at
fixed offsets of the FM-1's 1 MiB NOR (plus 3 user sample slots). The port keeps that map and
backs the window 0x90000..0xFFFFF with the `sloop` data partition (ESP32) or a file (host), with
NOR semantics. NVS holds ESP-IDF's own data, the network settings changed on the device and the
self-test's phase.

Backups: `GET /api/store` streams the window; a restore (`POST /api/store`) is checked with
`sloop_store_check` (storage.c's own header / CRC rules), staged in the `stage` partition with a CRC
trailer, and copied in by `sloop_port_store_init` at the next boot, before SLOOP starts.

SLOOP's clock (`sloop_plat_time_us`, hence `fm1_ms`) counts from this boot, as the FM-1's timer.

Firmware: two 4 MB OTA slots. `POST /api/ota` (device page) writes the other slot and restarts into
it; the bootloader rolls back an image that does not confirm itself (`sloop_ota_confirm`, once
SLOOP runs and the web server is up).

## Tests

Host: upstream's suite (unmodified), the port's boot / selftest / features / soak / protocol /
server / USB-MIDI bridge tests, the web panel tests (node), the WebSocket end-to-end test, the
browser tests (headless Chrome). QEMU: the firmware self-test
(two phases across a restart), the cross-target render hash, and the same WebSocket end-to-end
test against the firmware's web server over emulated Ethernet.
