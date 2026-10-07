# Next steps

## NEXT ACTION

Software milestones M0–M9 are complete and tested; M10 is HARDWARE READY; M11's software items
(USB-MIDI device, firmware update and projects backup from the browser) are done and the rest
needs the board.

The board is connected and plays (PROGRESS.md "Hardware results": H-1 verified, H-5 sound heard on
both channels, H-2..H-4 partial; the PCM5102A sits on adapter terminals whose S3 GPIOs were
measured). Continue with H-6 (sample rate), H-7 (underruns, CPU), then the rest of
`docs/HARDWARE_TESTS.md` in order
(`cp config/local.env.example config/local.env`, set pins + Wi-Fi, `scripts/flash.sh`,
`scripts/monitor.sh`; turn on `SLOOPY_USB_MIDI` only for H-13, flashing through the UART port),
record results in PROGRESS.md (M10, M11), file problems in KNOWN_ISSUES.md.

Without the board, the planned software work is done. Optional (each: implement, test on host +
QEMU, update docs, commit): TRS MIDI on a UART (upstream `midi_uart.c`, which upstream builds off)
for a DIN socket; pixel-level panel matching once the user provides a straight-on FM-1 photo
(docs/UI_SPEC.md); the panel on real phones / tablets.

Commands: `scripts/test-host.sh`, `scripts/test-qemu.sh`, `scripts/qemu-cpu-estimate.sh`,
`scripts/serve-web.sh` (panel at http://127.0.0.1:8080/, editor at /editor.html).

## After that

- Hardware results may call for tuning: I2S DMA depth, moving hot `.pool` buffers (reverb / delay
  lines) to internal RAM if PSRAM bandwidth shows up in H-7, Wi-Fi task priorities.

## Context a fresh session needs

- Read `docs/UPSTREAM_MAP.md` first: the port compiles upstream unmodified against a virtual HAL.
- Never edit `upstream/sloop-fm1` (gitignored, fetched at the locked commit).
- QEMU must run with stdin from /dev/null (KI-1). The shell's `CC` may be wrong (KI-2).
- QEMU profile uses quad PSRAM: octal crashes Espressif QEMU 9.2.2 (KI-5).
- SLOOP's MIDI / SysEx output has one reader per transport (`SLOOP_MIDI_WEB`, `SLOOP_MIDI_USB`):
  a new transport attaches with its own id and must keep reading (docs/ARCHITECTURE.md).
- QEMU's multi-threaded TCG can abort during heavy flash remapping (KI-9): the second QEMU boot
  (backup, OTA) runs single-threaded. Compare heap figures on a free `QEMU_WEB_PORT` (KI-8).
  QEMU keeps its timer across restarts (KI-10): detect restarts by `boot` in `/api/status`.
- Gate commits on the test command's own exit status (not on a `| tail`).
