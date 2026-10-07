# Progress

Operational state of the port. Statuses: NOT STARTED / IN PROGRESS / BLOCKED / COMPLETE.
Verification levels: BUILD VERIFIED, HOST TESTED, QEMU TESTED, HARDWARE READY, HARDWARE VERIFIED
(not interchangeable).

- Upstream SLOOP: `f2b44c219b8a4ac00bc06dca756cdae8a259dd1a` (SLOOP 2.2), see `config/upstream.lock`
- ESP-IDF: `v5.5.5` (`b774170ff4`), in `tools/esp-idf` (gitignored), tools in `~/.espressif`
- QEMU: Espressif `qemu-system-xtensa` 9.2.2 (esp_develop_9.2.2_20260417), machine `esp32s3`
- Host: Nobara (Fedora 44), GCC 16.2.1, CMake 4.3, Python 3.13 + Pillow, Node present

## M0 — Repository audit and development environment: COMPLETE

- Read CLAUDE.md, PROJECT.md, README.md, docs/*, scripts, configs, reference assets.
- Repo was not a git repository: `git init` + a scaffold commit.
- Installed (user space, no root): ESP-IDF v5.5.5 + esp32s3 toolchain (xtensa GCC 14.2.0) +
  qemu-xtensa via `tools/esp-idf/install.sh esp32s3` and `idf_tools.py install qemu-xtensa`.
- Scaffold fixes: `main` required a nonexistent `esp_flash` component (-> `spi_flash`);
  `build-qemu.sh` shared the root `sdkconfig` with the hardware build (-> per-build
  `-DSDKCONFIG=build-*/sdkconfig`).
- Environment quirks in KNOWN_ISSUES.md (KI-1 QEMU stdin, KI-2 CC=gcc-14, KI-3 packages).
- Remaining: `scripts/doctor.sh` / `bootstrap-nobara.sh` to be refreshed for the new scripts.

## M1 — Upstream SLOOP acquisition and architecture map: COMPLETE

- `scripts/fetch-sloop.sh` (checks out the locked commit), `upstream/SLOOP-LICENSE.txt` copied.
- Map: `docs/UPSTREAM_MAP.md` (every file, its role, reused / replaced / not built; hardware
  boundaries; concurrency model).
- Upstream tests: `scripts/test-upstream.sh` -> **21 passed, 3 skipped** (JieLi-only, KI-4),
  including the 97-render golden regression, bit-exact on x86-64 GCC 16. HOST TESTED.

## M2 — Native host foundation: COMPLETE

- Architecture: upstream `firmware/src` compiled unmodified in `components/sloop_core/port/sloop_unity.c`
  against the virtual FM-1 HAL `components/sloop_core/port/hal/*.h`; `usb.c` -> `sloop_usb_shim.c`,
  `main.c` -> `sloop_runtime.c`. Public API `components/sloop_core/include/sloop.h`; platform
  contract `sloop_platform.h`; Linux platform `host/platform_host.c`.
- Shared CMake `cmake/sloop_core.cmake` runs upstream's generators (Pillow python) per build.
- Build: `scripts/build-host.sh` (CMake, `host/CMakeLists.txt`) -> `build-host/`. BUILD VERIFIED.
- Test: `tests/host/test_port_boot.c` (+ harness `tests/host/sim.h`): boot logo, TRACKS screen,
  main loop, keys audible, PLAY starts/stops the transport, PLAY LED flashes on the beat, audio
  bit-identical across two power-ons. `SKIP_UPSTREAM=1 scripts/test-host.sh` -> PASS. HOST TESTED.

## M3 — Host audio and core functionality: COMPLETE (HOST TESTED)

- Genuine DSP path through audio.c's ISR via the virtual ALNK; deterministic audio (hash equal
  across power-ons and equal on the ESP32 in QEMU); WAV output (tests, `sloop_host --wav`); real
  audio on Linux via ALSA (`sloop_host`, 0 underruns over 3 s on this machine).
- Upstream's own regression suite (97 golden renders: every engine x preset, kits, FX, mix) runs
  unmodified (`scripts/test-upstream.sh`) and covers the DSP; the port adds:
  `tests/host/test_port_features.c` — through the virtual panel only, following SLOOP.md:
  ALGORITHM track select, PRESETS kit, drum keys, free take (tempo from playing: 120 BPM), loop
  playback, EDIT+OCT undo/redo, FX punch-in, GLO mute/unmute, SELECT tempo (exact + accelerated),
  autosave, power cycle (new process, same store file: tempo + pattern restored), hold REC clears;
  7 golden screen hashes (`tests/host/golden_screens.txt`, `GOLDEN_UPDATE=1` rewrites).

## M4 — ESP32-S3 firmware build foundation: COMPLETE (BUILD VERIFIED)

- Components: `sloop_core` (the same unity build as the host, `cmake/sloop_core.cmake`; linker
  fragment `linker.lf` puts SLOOP's `.pool` buffers in PSRAM `.ext_ram.bss`), `sloop_port`
  (ESP-IDF platform: esp_timer clock, FreeRTOS mutexes, store on the `sloop` partition with
  `esp_partition_*` + mmap; audio + UI tasks pinned to one core, audio prio 20 > UI prio 5),
  `audio_backend` (null paced output; I2S std TX 32-bit for the PCM5102A), `main` (boot, self-test).
- Partition table `config/partitions.csv`: `sloop` data 0x40 at 0x820000 (448 KiB = SLOOP's
  FM-1 map 0x90000..0xFFFFF), `userfs` reserved, OTA slots 4 MB each, coredump.
- Profiles: `sdkconfig.defaults` (+ `config/sdkconfig.qemu.defaults` | `config/sdkconfig.hardware.defaults`
  + generated `config/sdkconfig.local.defaults`). Hardware: octal PSRAM 80 MHz, CPU 240 MHz.
  QEMU: quad PSRAM (KI-5), CPU 160 MHz.
- Memory (QEMU build): internal DIRAM 143.7 KB used of 341.8 KB; SLOOP `.pool` 552,672 B in PSRAM;
  flash image 805 KB. Free internal heap at runtime ~286 KB.
- `scripts/build-qemu.sh` -> PASS, `scripts/build-hardware.sh` -> PASS (no warnings in project code).

## M5 — ESP32-S3 QEMU: COMPLETE (QEMU TESTED)

- `scripts/test-qemu.sh` -> **QEMU test: PASS** (`main/selftest.c`, 2 phases, ~45 s):
  cross-target render hash ESP32-S3 `274479377fec3840` == host `274479377fec3840` (bit-identical
  DSP/sequencer/mixer on Xtensa); 16 MB PSRAM + heap; SLOOP main loop and screens; PLAY on/off;
  key heard; KNOB 2 changes level + redraw; autosave writes the `sloop` partition (r/e/w 20/1/14,
  0 errors); heap integrity; esp_restart; project restored from flash (level 98 == 98).
- Measured in QEMU (not real timing): stack high-water free audio 4.9 KB / 6 KB, UI 6.2 KB / 8 KB.
- Not validated by QEMU (hardware only): I2S/PCM5102A, octal PSRAM init (KI-5), Wi-Fi, real
  render timing / CPU load.

## M6 — Full SLOOP feature port: COMPLETE for v1 scope (HOST TESTED; QEMU TESTED where noted)

- All upstream engines (9), drums (37 kits), sequencer, arranger/song mode, mixer, FX buses,
  punch-in FX, slicer, UI, user presets, 4 projects + autosave, editor SysEx protocol compile into
  the port unchanged (upstream SLICE engine stays off, as upstream builds it).
- Through the platform interfaces: audio (virtual ALNK), display (virtual ST7789), input (virtual
  matrix), storage (platform store: file / flash partition), clock, locks.
- Validated: upstream suite (DSP, sequencer, UI pages, storage formats) + port feature test +
  cross-target render hash on ESP32 (QEMU) + QEMU autosave/restart/restore.
- Not ported (FM-1 hardware only, documented in docs/UPSTREAM_MAP.md): M-UPGRADE OTA, USB
  recovery, CDC console, JieLi UBOOT entry, battery gauge, TRS MIDI (off upstream too).
- Transports for SLOOP's MIDI and editor SysEx: the WebSocket (M9) and the USB-MIDI device (M11).

## M7 — Virtual FM-1 input and display layer: COMPLETE (HOST TESTED, QEMU TESTED)

- Core: virtual matrix (buttons by label, keys, encoders, MASTER pot) wired as PANEL_DEFAULT
  (so SLOOP's holds / layers / long presses / calibration run unchanged), 20 ms minimum hold, one
  change per control per UI pass; virtual ST7789 framebuffer + dirty box; LED read-back; status.
- Protocol v1 (`docs/PROTOCOL.md`, `components/sloop_proto`): JSON control/state, binary screen
  rectangles, hub shared by host and ESP32; a disconnecting client releases what it held.
- Tests: `test_proto` (parser, encoders, hub, disconnect), `test_ws_server` (SHA-1/base64/RFC 6455),
  `tests/web/test_ws_e2e.py` (HTTP, WebSocket session, PLAY, LEDs, rectangles rebuild the exact
  screen, ping, errors, two panels) against the host app AND against the firmware in QEMU.

## M8 — High-fidelity FM-1 web panel: COMPLETE (visual fidelity: measured, not pixel-perfect)

- `web/panel-geometry.js`: every control measured on a straight-on FM-1 product photo (aspect
  1.676; overlay of the rendering on the photo: edges coincide within a few units). The photo is
  third-party and not committed (see docs/UI_SPEC.md for how to re-measure).
- `web/panel.js`: SVG components (knurled encoders, MASTER pot, backlit buttons in trays, pill
  keys with LED light lines, keybed marks), SLOOP's real screen in a canvas (crisp when >= 240 px,
  smoothed below: nearest-neighbour shrinking erased 1-px strokes), multi-touch holds, right-click
  latch, wheel/drag encoders, computer keyboard shortcuts, accessibility labels.
- Printing follows the hardware: the 2nd button reads "SEL" (SLOOP's SCL).
- Tests: `tests/web/test_panel.mjs` (geometry vs the FM-1 and the protocol, no overlaps, key
  layout, shortcuts, drag/wheel maths, screen messages, session). Screenshots checked headless.
- Not done: a user-owned high-resolution photo for pixel-level matching; real touch-device tests.

## M9 — ESP32 networking and persistence: IN PROGRESS (QEMU TESTED over Ethernet; Wi-Fi BUILD VERIFIED)

- `components/web_server`: Wi-Fi STA (8 retries, then its own WPA2 AP named after the hostname)
  or AP from `config/local.env`; OpenCores Ethernet under QEMU; esp_http_server with the panel
  gzip-embedded (6 files) and `/ws` through the same hub; pump task on core 0; panel sockets use
  a send-everything override (httpd's default send reports short writes as success).
- QEMU: the web e2e test passes against the firmware (`scripts/test-qemu.sh`, port 18080).
- Persistence: SLOOP's own store on the `sloop` partition (QEMU TESTED: autosave, restart, restore).
- SLOOP's own web editor (upstream `web/editor.html`, unmodified + one injected bridge line) is
  served at `/editor.html`; its Web MIDI SysEx goes over the WebSocket to `editor.c` (QEMU TESTED:
  INFO, SET/GET, SMP_INFO, PING; HOST TESTED in Chrome: connects and reads every parameter).
- mDNS: `http://<hostname>.local/` (managed `espressif/mdns` 1.14, hardware profile).
- Runtime network settings: `/device.html` + `/api/settings` (NVS over local.env, guarded by the
  current password; QEMU TESTED: read, refuse, validate, persist, reset). `/api/status` diagnostics.
- Remaining: Wi-Fi on hardware (H-4).

## M10 — PCM5102A hardware-ready backend: HARDWARE READY (BUILD VERIFIED; output UNVERIFIED)

- `components/audio_backend`: ESP-IDF `i2s_std` TX, Philips I2S, 32-bit slots (SLOOP's 24-bit
  left-justified), stereo, 44.1 kHz, MCLK unused (PCM5102A SCK to GND), DMA 4 x 256 frames
  (23 ms), `auto_clear` (silence on underrun / flash write), staging buffer in internal RAM.
  Pins from `config/local.env` (`SLOOPY_I2S_*`); `-1` pins -> no output, said on the console.
- Audio task: core 1, prio 20, renders a half (256 frames) then blocks in `i2s_channel_write`:
  the I2S clock paces SLOOP. SLOOP's own overload guard (voice shedding at 85 %) stays active.
- Memory: voices / tracks / mixer state, `abuf`, DMA in internal RAM; FX lines etc. in PSRAM.
- CPU estimate (not a measurement): `scripts/qemu-cpu-estimate.sh` counts Xtensa instructions with
  QEMU `-icount`: the 4-track self-test song costs 70.5 M instructions per second of audio (1599 per
  stereo frame) -> 29 % (CPI 1.0) .. 44 % (CPI 1.5) of one 240 MHz core.
- Validation procedure: `docs/HARDWARE_TESTS.md` (wiring, safe GPIOs, H-1..H-12). Every audio
  item is UNVERIFIED until run on the board.

## M11 — Integration and release-ready development state: IN PROGRESS

- USB-MIDI device (FM-1 parity: MIDI in / out, SLOOP's editor over Web MIDI): HOST TESTED (bridge),
  BUILD VERIFIED (both firmware variants), hardware UNVERIFIED (H-13).
  - Core: SLOOP's MIDI-out and SysEx rings have one reader per transport (`SLOOP_MIDI_WEB`,
    `SLOOP_MIDI_USB`; `sloop_usb_shim.c`); writers see the slowest attached reader, editor.c's
    `ed_room()` too. Every attached transport gets every reply / push / note.
  - `components/usb_midi`: portable bridge (packets <-> SLOOP; waits 200 ms for a host that stops
    reading, as usb.c, then drops instead of holding up the web) + TinyUSB device (esp_tinyusb
    2.4.0 / tinyusb 0.21.0, pinned), core 0, VID:PID 1209:0001 as upstream, product "Felucca
    (sloopy-sloop-32)", serial = MAC. Configuration descriptor checked in the ELF: byte-identical to
    upstream usb.c's 101-byte CFG_DESC.
  - Opt-in (`SLOOPY_USB_MIDI=1` in local.env): TinyUSB takes the native USB port (KI-6).
  - `tests/host/test_usb_midi.c` (22 checks; a mutant without the stall drop fails 2 of them);
    `/api/status` + device page + console report the link and counts (e2e: off on host and QEMU).
  - Memory: per-transport SysEx buffers and the bridge state in PSRAM; boot internal free in QEMU
    232,819 B (was 232,699).
- Firmware update from the browser: QEMU TESTED (API and device page in Chrome), hardware
  UNVERIFIED (H-14).
  - `components/web_server/ota.c`: `POST /api/ota` (raw image, `X-Sloopy-Password`), first 4 KB
    checked (image magic, ESP32-S3, this project), written sequentially into the other slot,
    `esp_ota_end` verifies, boot slot switched, restart. Bootloader rollback on; the new image
    confirms itself in app_main once SLOOP runs and the web server is up (else rolls back).
  - `/api/status` -> `app` (version, build time, slot, OTA state); device page: Firmware section
    (upload progress, waits for the restart, reports the new version or a rollback).
  - `scripts/test-qemu.sh`: second boot of the same flash (the passed self-test stays idle), with
    single-threaded TCG (KI-9): `tests/web/test_ota.py` (wrong password, not an image, another
    project, truncated -> refused, no restart; the image -> ota_1, confirmed, panel answers), then
    `test_browser.py --connect` updates back to ota_0 through the device page. 1132 KB in ~11 s.
- Projects backup / restore from the browser: HOST TESTED (format check), QEMU TESTED (API and
  device page in Chrome), hardware UNVERIFIED (H-15).
  - Core: `sloop_store_check` reads a store image with storage.c's own rules (A/B headers, CRCs)
    and eng_sample.c's slot header checks; `tests/host/test_store_backup.c` (backup of an autosaved
    tempo, damaged copy / header, blank, short, other data refused, restored onto a fresh store:
    the tempo comes back).
  - `GET /api/store` streams the window; `POST /api/store` checks, stages into the new `stage`
    partition (512 KB from `userfs`) with a CRC trailer and restarts; `sloop_port_store_init`
    copies it in before SLOOP starts. Device page: download link, restore form.
  - `tests/web/test_store.py` in QEMU's second boot: tempo A autosaved, backup, tempo B autosaved,
    refusals, restore -> A; then `test_browser.py --connect --backup` through the page.
  - QEMU findings (KI-10): its systimer survives `esp_restart` -> SLOOP's clock now counts from each
    boot; `/api/status` gained `boot` (NVS counter) for restart detection; PSRAM-sourced partition
    writes go through an internal sector buffer (96 s -> 4 s in QEMU).
- Done: `scripts/test-host.sh` (layout 7, upstream 21+3 skipped, port 8 suites incl. headless
  Chrome), `scripts/test-qemu.sh` (self-test + cross-target hash + web/editor e2e), docs rewritten
  to match reality (README, ARCHITECTURE, CONFIGURATION, EMULATION, UI_SPEC, PROTOCOL,
  HARDWARE_TESTS, ACCEPTANCE), doctor / bootstrap / build / flash / monitor / clean scripts.
- Remaining: hardware validation (H-1..H-15), then record results here.

### Hardware results

Board (2026-10-07, read by esptool and the boot log): an ESP32-S3 module without headers (rev
v0.2, embedded 16 MB octal PSRAM, AP Memory 1.8 V; 32 MB Macronix **octal** flash, eFuse: octal)
in an adapter / programming board made for the classic ESP32-WROOM-32 (its USB-UART is a CH343).
The quad-flash build runs on it unchanged: `ESPTOOLPY_FLASH_MODE_AUTO_DETECT` switches to
`opi_str`; the 16 MB layout uses the first half.

The adapter's labels are the WROOM-32's, not the S3's GPIOs. Measured with `scripts/probe-pins.sh`
(a terminal touched to GND -> the GPIO that reads low): IO33 -> GPIO 16, IO34 -> GPIO 6,
IO35 -> GPIO 7, IO18 -> GPIO 39; IO16 and IO17 reach no GPIO (on the S3 module those pads are
IO36 / IO37, the octal PSRAM's); one of its GND contacts lands on GPIO 46. GND, 3V3, EN, TX, RX
line up. This fits the S3-WROOM pinout lying where the WROOM-32's was (docs/HARDWARE.md).
PCM5102A wired: BCK -> IO33 (GPIO 16), LCK -> IO34 (GPIO 6), DIN -> IO35 (GPIO 7), VIN 3V3.

- H-1 boot report: **HARDWARE VERIFIED** (2026-10-07). `profile hardware`, flash 16777216, PSRAM
  16777216, `.pool` in PSRAM 566724 B, CPU 240 MHz, `I2S TX: 44100 Hz, 32-bit stereo, BCLK 16,
  WS 6, DOUT 7, DMA 4 x 256 frames`, `SLOOPY_APP_READY`, `SLOOP_BOOTED`; boots from `ota_0`,
  `ota_state` valid.
- H-2: octal PSRAM at 80 MHz initialises, its memory test passes (`SPI SRAM memory test OK`); the
  30 min stability run is pending.
- H-3 / H-4 (partial): STA joined (WPA2, RSSI -48), DHCP address, mDNS `sloopy.local`; the panel
  page and `/api/status` answer over Wi-Fi and play SLOOP from a browser. The AP fallback is
  pending.
- H-5 (sound): **heard** (2026-10-07, user report): a key on the panel sounds from the PCM5102A on
  both channels. The first PCM5102A board gave one channel only; a second board gives both (the
  firmware sends identical L / R for a centred sound: the host render of the same core has
  L == R on every frame). Comparing the sound with the host app is pending.
- Idle figures: cpu 8-9 % stopped, render max 1029-1066 us, internal heap free 73 KB (min 37.5 KB).
- Found: `underruns` read 101 after boot and stayed there: the ~600 ms between enabling I2S and
  SLOOP's first block (the DMA plays its cleared buffers). The counter now starts at the first
  write, so H-7 reads real underruns only (on the board: `underruns 0` after boot).

## History

The development history (29 commits: scaffold import, then M0-M11 and the first hardware bring-up)
was squashed into one commit on 2026-10-07 before publishing. The milestones above record what
each stage did and how it was tested; new work is committed on top as usual.
