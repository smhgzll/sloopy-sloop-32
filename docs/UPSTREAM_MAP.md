# Upstream SLOOP architecture and port map

Upstream: <https://github.com/isod89/sloop-fm1>, commit `f2b44c219b8a4ac00bc06dca756cdae8a259dd1a`
("SLOOP 2.2", recorded in `config/upstream.lock`). License: GPL-3.0-only, assets per upstream
`LICENSING.md` (Terminus font OFL, CC0 samples, Hügelton icon atlas with a GPL §7 permission).

## How upstream is built

- One compilation unit: `firmware/src/felucca.c` `#include`s every `.c` file in a fixed order.
- All hardware access lives in header-only `firmware/hal/fm1_*.h` (JieLi AC79 / FM-1 registers).
  Upstream's `tools/build.py` enforces this (`mmio_check`: no register literals, volatile casts or
  inline asm outside `hal/`).
- Generated headers (`build/gen/*.h`) come from `tools/gen_*.py` (Python + Pillow): font, icons,
  tables, ADPCM samples, drum kits, logo. They need no JieLi toolchain.
- Upstream's host tests (`tests/*.c`) include the sources directly with small doubles.

## Port strategy

Upstream `firmware/src/` is compiled **unmodified**, in `felucca.c`'s order, by
`components/sloop_core/port/sloop_unity.c`, against a **virtual FM-1 HAL**
(`components/sloop_core/port/hal/`, first on the include path) instead of `firmware/hal/`.
The same unity file is built for the Linux host and for the ESP32-S3.

## File-by-file map

| Upstream | Role | Port status |
| --- | --- | --- |
| `core.h` | tracks, voices, engines, parameters, transport clock | reused unchanged |
| `engines.c`, `dsp.c`, `eng_*.c` (analog, digital/FM, phase, lofi, sample, formant/voice, trio, drawbar, grain; slice off upstream too) | 9 synth engines | reused unchanged |
| `drums.c`, `drum_synth.c` | drum track, 32 synth kits + sampled kit | reused unchanged |
| `voice.c` | voice allocation, envelopes, LFO | reused unchanged |
| `fx.c`, `punch.c`, `slicer.c` | DIST, chorus/delay/reverb buses, master DUST/DUCK/filter, punch-in FX, slicer | reused unchanged |
| `seq.c`, `arranger.c`, `arranger_scene.c` | keyboard layers, scales/chords, arp, sequencer, recording, song arranger (runs in the audio ISR) | reused unchanged |
| `params.c` | parameter descriptors, pages | reused unchanged |
| `ui.c`, `ui_draw.c`, `ui_studio.c`, `ui_layers.c`, `ui_menu.c`, `ui_song.c`, `ui_input.c`, `icons.c`, `splash.c`, `gfx.c` | the whole UI: input semantics (holds, layers, long press, combos), screens | reused unchanged |
| `lcd.c` | ST7789 command protocol | reused unchanged, talks to the virtual ST7789 |
| `panel.c` | label <-> matrix wiring, calibration table, settings | reused unchanged |
| `audio.c` | the ALNK0 audio ISR, voice shedding, CPU load | reused unchanged, driven by the virtual ALNK |
| `storage.c`, `project.c`, `upreset.c` | A/B flash store, projects (4 + autosave), 32 user presets | reused unchanged on the platform store |
| `editor.c` | web editor SysEx protocol (v5) | reused unchanged; its frames come and go over the WebSocket (`sysex`); upstream `web/editor.html` is served with a Web MIDI bridge |
| `libc.c` | freestanding memset/memcpy/memcmp + small string helpers | reused (its mem* renamed, as upstream host tests do) |
| `usb.c` | register-level USB-MIDI (+CDC) device | **replaced** by `port/sloop_usb_shim.c` (same MIDI / SysEx rings and API, one reader per transport) + `components/usb_midi` (TinyUSB, the same USB-MIDI configuration descriptor) |
| `main.c` | FM-1 boot, IRQ wiring, main loop | **replaced** by `port/sloop_runtime.c` (`felucca_init` copied verbatim) |
| `ota.c`, `recovery.c` | M-UPGRADE firmware update, USB recovery | not built (FM-1 specific); the ESP32 has its own update from the device page (`components/web_server/ota.c`, ESP-IDF OTA slots + bootloader rollback) |
| `console.c` | CDC serial console | not built (FELUCCA_CDC=0) |
| `midi_uart.c` | TRS MIDI IN (off upstream too) | not built yet (its parser can feed `midi_in_q` later) |
| `firmware/hal/*.h` | JieLi registers | **replaced** by `port/hal/*.h` (virtual devices) |
| `crt0.S`, `app.ld`, `hal/*.S`, `loader/` | JieLi startup, linker map, ISR stubs, update loader | not used |

## Hardware boundaries and their virtual replacements

| FM-1 hardware | Upstream HAL | Port |
| --- | --- | --- |
| TIMER4 24 MHz time base | `fm1_time.h` | platform µs clock (`FM1_TICKS_PER_US 1`) |
| TIMER5 10 kHz ISR (input scan, USB poll, `fm1_ms`) | `fm1_timer.h`, `main.c` | `sloop_ui_step()` / `sloop_port_idle()` in the UI context |
| IRQ enable/disable | `fm1_irq.h` | platform audio lock (mutex, cli/sti semantics) |
| watchdog, reset, UBOOT | `fm1_sys.h` | no-ops / `sloop_plat_reboot()`; feeding the WDT runs the port idle service |
| stack/write/PC guards | `fm1_guard.h` | no-ops (ESP-IDF / OS protections) |
| key/button/encoder matrix + LEDs | `fm1_input.h` | virtual matrix: `fm1_virt_key/enc`, LED read-back |
| MASTER pot, battery ADC | `fm1_adc.h` | virtual pot (default = upstream's start gain), no battery |
| ST7789 240x240 SPI LCD | `fm1_lcd_hw.h` | virtual ST7789 (CASET/RASET/RAMWR) -> RGB565 BE framebuffer + dirty box |
| ALNK0 I2S + DMA double buffer | `fm1_audio.h` | virtual ALNK: the platform audio driver pulls halves; `audio.c` ISR runs unchanged |
| SPI NOR (1 MiB) + XIP | `fm1_flash.h`, `fm1_xip.h` | platform store for FM-1 offsets 0x90000..0xFFFFF (partition / file), NOR semantics |
| USB device | `fm1_usb.h` | shim + transports: WebSocket hub, USB-MIDI device (TinyUSB, opt-in); UART MIDI later |

## Shared state and concurrency

On the FM-1 the audio ISR preempts the main loop on one core; SLOOP protects multi-field updates
with `fm1_irq_off/on` and uses single-producer/single-consumer rings with `RING_PUBLISH()` for the
rest. The port keeps that model: the UI context and the audio context share one core on the ESP32
(audio at higher priority), `fm1_irq_off` is a mutex the audio render also takes, and transports
only use the I/O mailboxes in `sloop.h`.

## Upstream tests

`scripts/test-upstream.sh` runs every upstream host test that does not need the JieLi toolchain
(22 of 26 entries of upstream `tests/run_tests.sh`, including the 97-render golden regression).
Skipped, because they need `build/felucca.fwsc` / `felucca.dis` from the JieLi build:
`ota_test` (M-UPGRADE), `ldr_test` (update loader), `target_budget.py` (pi32v2 instruction
budget). None of those cover code the port builds.
