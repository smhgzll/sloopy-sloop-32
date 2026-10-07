# Claude project instructions — sloopy-sloop-32

Treat this file as the authoritative engineering brief for this repository.

## Goal

Port the open-source SLOOP firmware from the M-VAVE FM-1 to an ESP32-S3 N16R16 class board with a PCM5102A DAC, using ESP-IDF.

The project must ultimately provide the full practical SLOOP experience while replacing the physical FM-1 front panel with a browser-based panel that closely matches the real device.

## Non-negotiable decisions

1. Use **ESP-IDF**, not Arduino.
2. This is a **native port**, not a JieLi/FM-1 CPU emulator.
3. v1 hardware is only ESP32-S3 N16R16 + PCM5102A.
4. No SD card in v1.
5. No physical screen/buttons/encoders/pads in v1.
6. Browser UI is the control surface and virtual screen.
7. Do not hard-code Wi-Fi credentials or board pins.
8. Keep all secrets and local pin mappings outside tracked source files.
9. Preserve upstream SLOOP licensing and attribution. SLOOP is GPL-3.0-only; derivative code must remain compatible.
10. Do not remove or weaken upstream behavior just to make the first build pass. Stub hardware boundaries, not musical features.

## Development targets

Maintain three targets:

### 1. Native host tests

Use this for fast DSP/sequencer/UI logic tests. Reuse the upstream SLOOP host tests first, then add port-specific host tests.

### 2. ESP32-S3 QEMU

Use Espressif's ESP32-S3 QEMU for firmware boot, FreeRTOS, memory, flash/partition, PSRAM, tasking, timers and crash testing.

Baseline QEMU model:

- machine: `esp32s3`
- flash image: 16 MB
- PSRAM: 16 MB
- octal PSRAM mode

Do not pretend QEMU validates real I2S audio timing or Wi-Fi coexistence. Those are hardware integration tests.

### 3. Real hardware

Only after the software targets are healthy, validate:

- I2S -> PCM5102A
- 44.1 kHz realtime stability
- DMA underruns / pops / clicks
- PSRAM cache/bandwidth behavior
- Wi-Fi + WebSocket + audio at the same time
- USB/MIDI later

## Architecture rule

SLOOP core code must never directly depend on ESP-IDF GPIO/I2S/Wi-Fi APIs.

Create narrow interfaces for:

- audio output
- virtual display
- user input events
- storage
- monotonic clock/timers
- MIDI
- web transport

Expected direction:

```text
SLOOP core
  -> platform interfaces
       -> host backends
       -> qemu-safe ESP32 backends
       -> real ESP32 hardware backends
```

## Audio

Target 44.1 kHz stereo, preserving upstream fixed-point DSP where practical.

The hot realtime path must avoid uncontrolled allocation and blocking. Keep DMA buffers and hot DSP state in internal RAM. Large samples/wavetables/assets may use PSRAM after measurement.

PCM5102A is output only. Do not add unnecessary codec abstractions that imply audio input unless needed later.

## Browser UI

The browser panel must not be a generic synth dashboard.

Use `assets/references/mwave-fm1-front-reference.*` plus the source image URL and any user-provided photos/screenshots as geometry references. Reproduce:

- overall panel aspect ratio
- display location/size
- knob/encoder locations
- button matrix and transport layout
- keyboard/pad layout
- labels and spacing
- SLOOP screen/menu behavior

The project may use HTML/CSS/SVG/Canvas/WebGL as appropriate, but the visual layout should remain responsive while keeping the physical panel proportions.

Prefer transporting the real SLOOP display state/framebuffer to the browser instead of reimplementing the screen logic twice.

Browser controls must emit semantic input events such as button press/release, encoder delta, long press, shift/layer combinations, pad/key press and velocity where SLOOP supports them.

## Web transport

Use a persistent low-latency channel (normally WebSocket) for realtime control/state. Do not implement realtime interaction as a pile of REST calls.

Keep binary display/frame/audio-adjacent payloads binary where useful. Keep control messages versioned and easy to inspect.

## Configuration

Use ESP-IDF Kconfig/sdkconfig for compile-time target configuration and NVS/runtime configuration where appropriate.

The user-editable local file is `config/local.env`, generated from `config/local.env.example`. It is gitignored.

`scripts/generate-local-sdkconfig.sh` turns local settings into an untracked sdkconfig overlay.

Never commit the user's Wi-Fi password.

## Upstream SLOOP

Fetch with:

```bash
./scripts/fetch-sloop.sh
```

Default repository:

`https://github.com/isod89/sloop-fm1.git`

Do not silently vendor a stale copy. Keep upstream under `upstream/sloop-fm1/`, record the tested commit, and make the port layer explicit.

Before changing upstream logic:

1. run upstream tests;
2. identify JieLi/FM-1 dependencies;
3. document them;
4. introduce platform seams;
5. port incrementally;
6. keep regression tests.

## Required quality gates

A change is not "done" merely because it compiles.

For each milestone:

- format/lint where available
- run host tests
- build ESP32-S3 QEMU target
- run QEMU smoke/integration tests
- update documentation when architecture/config changes

When real hardware is available, also run the relevant hardware integration tests.

## Do not do these

- Do not switch the project to Arduino.
- Do not emulate the FM-1 CPU to avoid porting.
- Do not hard-code one clone board's pins globally.
- Do not add SD card dependency to the first milestone.
- Do not redesign the web UI into an unrelated modern synth UI.
- Do not claim QEMU validates I2S/Wi-Fi hardware behavior when it does not.
- Do not delete difficult SLOOP features to create a demo-only port.

## First implementation task

Start by executing `scripts/doctor.sh`, fetching SLOOP, running its tests, producing an upstream architecture/dependency map, and making the existing ESP-IDF scaffold boot in QEMU. Then proceed through `docs/PORT_PLAN.md`.
