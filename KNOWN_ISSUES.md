# Known issues

Unresolved or noteworthy technical issues. Check here before debugging something that looks
familiar. Each entry: symptom, cause, evidence, attempts, workaround, blocking.

## KI-1: QEMU dumps core when its stdin is not a terminal or /dev/null

- **Symptom:** `timeout 12 qemu-system-xtensa ... | tail` ends with "the monitored command dumped
  core" right after `boot: Loaded app from partition`.
- **Cause:** `-nographic` puts the serial console on stdio; with a pipe or a closed stdin from a
  non-interactive shell (Claude Code, CI) QEMU crashes.
- **Evidence:** the same command with `< /dev/null > log 2>&1` boots to `QEMU_SMOKE_READY`.
- **Workaround:** the QEMU scripts always run QEMU with `< /dev/null` and the serial output in a
  log file (or `-serial file:` / `-serial tcp:`).
- **Blocks:** nothing (worked around).

## KI-2: the shell exports `CC=gcc-14`, which is not installed

- **Symptom:** `cmake -S host -B build-host` fails: "Could not find the compiler specified in the
  environment variable CC: gcc-14".
- **Cause:** user shell profile; the machine has GCC 16.
- **Workaround:** `scripts/common.sh host_cc` picks `$SLOOPY_HOST_CC`, `$CC` if installed, else
  `cc`/`gcc`/`clang`; `scripts/build-host.sh` exports it.
- **Blocks:** nothing (worked around). The user may want to fix their profile.

## KI-3: some Fedora packages named by the original bootstrap are not installed

- `git-lfs wget gperf ccache dfu-util` are missing; none is needed for building or QEMU.
  `SDL2` is provided by `sdl2-compat` on Fedora 44 (QEMU links fine). Passwordless sudo is not
  available to the agent, so nothing was installed system-wide. `scripts/bootstrap-nobara.sh`
  prints the exact `dnf` command for the user.

## KI-4: upstream tests that need the JieLi toolchain are skipped

- `ota_test`, `ldr_test`, `tests/target_budget.py` need `build/felucca.fwsc` / `build/felucca.dis`
  from the JieLi pi32v2 build. They cover the FM-1 update path and pi32v2 instruction budgets,
  which the port does not build. Not a blocker.

## KI-5: Espressif QEMU crashes on any octal-PSRAM data access

- **Symptom:** QEMU segfaults right after `boot: Loaded app from partition` (or later, at the
  first PSRAM access) when the firmware uses octal PSRAM and QEMU runs with
  `-global driver=ssi_psram,property=is_octal,value=true`.
- **Cause:** a bug in `qemu-system-xtensa` esp_develop_9.2.2_20260417: the core dump stack is
  `psram_quad_read <- ssi_transfer <- esp32s3_spi_txrx_buffer <- esp32s3_spi_special_command`.
- **Evidence (scratch experiments on the scaffold):** octal PSRAM + a 1 MiB `heap_caps_malloc`
  write: crash. Octal + `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` (64 KiB ext BSS): crash
  (the startup zeroes .ext_ram.bss). Quad PSRAM 16 MB + 600 KiB ext BSS, without `is_octal`:
  works. The original scaffold "booted with octal PSRAM" only because it never touched PSRAM
  (it read the heap size).
- **Workaround:** the QEMU profile (`config/sdkconfig.qemu.defaults`) uses `CONFIG_SPIRAM_MODE_QUAD`
  and `scripts/run-qemu.sh` runs QEMU without `is_octal`. Same 16 MB, same heap / ext BSS / MMU /
  cache configuration above the MSPI driver. The hardware profile keeps octal (N16R16).
- **Not validated in QEMU:** the octal PSRAM init of the real board (hardware test H-2).
- **Blocks:** nothing. Retry octal with a newer Espressif QEMU release.

## KI-6: with `SLOOPY_USB_MIDI=1` the native USB port is no longer a console / flash port

- **Symptom:** after flashing a USB-MIDI build over the board's native USB port, `scripts/monitor.sh`
  on that port shows nothing and `scripts/flash.sh` cannot connect.
- **Cause:** by design: TinyUSB takes the ESP32-S3's USB PHY (GPIO19/20) from the USB-Serial/JTAG
  controller to be the USB-MIDI device, as the FM-1's USB-C port is its MIDI port.
- **Workaround:** flash and monitor through the board's UART port ("COM", a USB-UART bridge on
  UART0), or enter download mode (hold BOOT, tap RESET / plug in) to flash over native USB.
  `SLOOPY_USB_MIDI` defaults to 0 for that reason.
- **Blocks:** nothing (documented in docs/CONFIGURATION.md, H-13).

## KI-7: two editors at once see each other's replies

- **Symptom:** with SLOOP's editor open twice (two browsers, or the web editor and one over USB),
  a request's reply also reaches the other editor.
- **Cause:** SLOOP's editor protocol has no request ids; editor.c handles one frame at a time and
  the port sends every reply / push to every attached transport (as several editors on the FM-1's
  single USB link would see). The editor matches replies by command byte, so simultaneous
  identical commands from two editors could take each other's reply.
- **Workaround:** use one editor at a time (pushes keep a second one in sync anyway).
- **Blocks:** nothing.

## KI-8: the QEMU run's "heap internal min" reads ~11 KB lower on port 18080 on this machine

- **Symptom:** `scripts/test-qemu.sh` reports `heap internal min ~101 KB` instead of ~112 KB.
- **Cause:** another local client (an open panel tab, the editor's port forwarding) connects to
  `127.0.0.1:18080` while the firmware runs: each extra HTTP / WebSocket client holds lwIP and
  hub buffers in internal RAM (the QEMU profile keeps lwIP internal).
- **Evidence (2026-10-06):** the same build: 101.5 KB on 18080, 112.2 KB with `QEMU_WEB_PORT=18091`;
  the previous commit gives 112.05 KB on a free port. Not a leak.
- **Workaround:** compare heap figures with `QEMU_WEB_PORT` set to a port nothing else uses.
- **Blocks:** nothing.

## KI-9: Espressif QEMU's multi-threaded TCG aborts while the firmware verifies an OTA image

- **Symptom:** during `POST /api/ota` (in `esp_ota_end`, `esp_image` reading the new slot's
  segments) QEMU dies: `qemu-system-xtensa: Bad ram pointer 0x42013097` (abort) or a SIGSEGV in
  translated code; the HTTP client sees "Remote end closed connection without response".
- **Cause:** a race in qemu-system-xtensa esp_develop_9.2.2_20260417 with one host thread per
  emulated core (MTTCG, the default): one core remaps flash MMU pages while the other executes code
  from the flash-mapped instruction bus (0x42xxxxxx). A host crash, not a firmware fault (no
  Guru Meditation in the log).
- **Evidence (2026-10-07):** full verifications of a 1.1 MB image (last byte corrupted, so no
  restart) in one session: multi-threaded TCG aborted at the 2nd of 20 in one session, survived 20
  in another; 1 of 6 full test runs crashed. Single-threaded TCG (`-accel tcg,thread=single`):
  60 of 60 in three sessions, and every full run since.
- **Workaround:** `scripts/test-qemu.sh` runs the update step in a second boot with
  `QEMU_TCG_THREAD=single` (`scripts/run-qemu.sh` option); the self-test and the protocol test keep
  multi-threaded TCG (real parallelism between the cores).
- **Blocks:** nothing. Retry multi-threaded TCG with a newer Espressif QEMU.

## KI-10: Espressif QEMU keeps its system timer across `esp_restart`; PSRAM-sourced flash writes crawl

- **Symptoms:** after a software restart under QEMU, log timestamps and `esp_timer` continue from
  the previous boot (`I (110740)` right after the reset); writing 448 KiB to a partition from a
  PSRAM buffer took ~96 s (4 s from an internal buffer).
- **Causes:** QEMU does not reset the systimer on the restart (a chip does); the flash driver
  bounces a non-internal source in small pieces, each a slow emulated flash operation.
- **Fixes in the port:** SLOOP's clock counts from its first use at each boot (platform_esp32.c),
  so SLOOP's "20 s after boot" logic and `uptime_ms` are right on both; `/api/status` has a `boot`
  counter (NVS) that clients use to see a restart; partition copies go sector by sector through an
  internal 4 KiB buffer (also less work for the flash driver on the chip).
- **Blocks:** nothing.
