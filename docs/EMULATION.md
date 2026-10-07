# ESP32-S3 emulation (QEMU)

The firmware (not the FM-1) runs in Espressif's QEMU fork: `qemu-system-xtensa`
esp_develop_9.2.2_20260417, machine `esp32s3`, installed by `scripts/bootstrap-nobara.sh`.

```bash
./scripts/test-qemu.sh     # automated: PASS / FAIL
./scripts/run-qemu.sh      # interactive console (Ctrl-A X quits); panel at http://127.0.0.1:18080/
KEEP_FLASH=1 ./scripts/run-qemu.sh   # keep the emulated flash (store, NVS) from the last run
```

Command line used (`scripts/run-qemu.sh`):

```text
qemu-system-xtensa -nographic -machine esp32s3 -m 16M
  -drive file=build-qemu/flash_image.bin,if=mtd,format=raw        (16 MB, merge_bin)
  -nic user,model=open_eth,hostfwd=tcp:127.0.0.1:18080-:80        (the web panel)
```

Run non-interactively with stdin from `/dev/null` (KNOWN_ISSUES KI-1).

## What it validates (and the test that shows it)

| Validated in QEMU | How |
| --- | --- |
| boot, ESP-IDF startup, partitions, 16 MB flash | boot log, partition table |
| 16 MB PSRAM: heap, `.ext_ram.bss` (SLOOP's `.pool`, 552 KB) | self-test, boot report |
| FreeRTOS tasks: SLOOP audio + UI on core 1, network on core 0 | self-test (UI frames, audio halves), stack high-water marks |
| SLOOP's DSP / sequencer / mixer compiled for Xtensa | cross-target render hash == host hash |
| SLOOP's UI and input semantics on target | self-test: PLAY, key heard, KNOB 2 level, screens |
| flash store: SLOOP's A/B storage on the `sloop` partition | autosave writes, `esp_restart`, project restored |
| NVS | the self-test's phase survives the restart |
| HTTP + WebSocket web panel, embedded gzip assets, protocol | `tests/web/test_ws_e2e.py` over the emulated Ethernet |
| heap integrity | `heap_caps_check_integrity_all` |

## What it does not validate

- **I2S / PCM5102A**: not emulated. The QEMU profile uses the null audio output paced by the clock.
- **Real-time behaviour**: QEMU runs instructions at its own speed; render times and CPU load there
  say nothing about the chip at 240 MHz.
- **Octal PSRAM**: this QEMU crashes on any octal-PSRAM data access, so the QEMU profile uses quad
  mode (same size and memory map; KI-5). The board's octal init is a hardware test.
- **Wi-Fi** and its coexistence with audio: QEMU has no radio; the panel runs over OpenCores Ethernet.
- **Firmware update**: QEMU validates the whole path (upload, image checks, writing and verifying
  the other slot, the restart, the new image confirming itself, the device page driving it in
  Chrome); not the time it takes over Wi-Fi or its effect on audio (H-14). That step runs with
  single-threaded TCG: this QEMU's multi-threaded TCG can abort while one core remaps flash pages
  the other executes (KI-9).
- **Store backups**: QEMU validates download, checks, staging and the copy at boot; Espressif's
  QEMU keeps its system timer across `esp_restart` (a chip keeps nothing), so the port's SLOOP clock
  counts from each boot and clients tell boots apart by `boot` in `/api/status` (KI-10).
- **USB-MIDI** (`SLOOPY_USB_MIDI`): QEMU has no USB OTG controller; the QEMU profile builds the
  component without starting it. Its bridge logic is host tested (`test_usb_midi`); enumeration
  and transfers are hardware test H-13.
- **Cache / PSRAM bandwidth, flash write stalls**: hardware measurements.

These are listed in [HARDWARE_TESTS.md](HARDWARE_TESTS.md).
