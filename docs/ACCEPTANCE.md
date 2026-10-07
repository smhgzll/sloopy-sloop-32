# Acceptance criteria

Status as of the last update of PROGRESS.md; `[x]` = met and tested, `[ ]` = open,
`[hw]` = needs the real board (docs/HARDWARE_TESTS.md).

## Upstream behavior

- [x] Upstream SLOOP commit is recorded (`config/upstream.lock`).
- [x] Upstream host tests pass (`scripts/test-upstream.sh`: 21 pass, 3 JieLi-only skipped).
- [x] Port-specific regression tests cover changed behavior (`tests/host`, `tests/web`).
- [x] Feature map: `docs/UPSTREAM_MAP.md` (every file: reused / replaced / not built).

## QEMU

- [x] ESP32-S3 target builds under the pinned ESP-IDF v5.5.5.
- [x] 16 MB flash image boots.
- [x] 16 MB PSRAM emulation (quad mode: octal crashes this QEMU, KNOWN_ISSUES KI-5).
- [x] Self-test passes (`SLOOP_QEMU_PASS`), no panic / reset loop (`scripts/test-qemu.sh`).
- [x] Core integration tests run without I2S hardware (null audio backend, cross-target hash).

## Web UI fidelity

- [x] Panel aspect ratio matches the reference (1.676, measured on a product photo).
- [x] Display position/size matches the reference (overlay check).
- [x] Knob/button/key positions match the reference (overlay: edges within a few units).
- [x] Proportions do not drift at different sizes (one SVG coordinate system, aspect-ratio box).
- [x] Every SLOOP control is reachable (14 buttons, 7 encoders, MASTER, 27 keys; multi-touch, latch).
- [x] Hold / shift / long press handled by SLOOP itself (edges with hardware timing).
- [x] The virtual screen is SLOOP's own framebuffer.
- [ ] Pixel-level match against a user-owned high-resolution photo.
- [ ] Tested on real touch devices (phone / tablet).

## Real audio [hw]

- [ ] PCM5102A outputs clean stereo audio at 44.1 kHz (H-5, H-6).
- [ ] No persistent underruns (H-7).
- [ ] No clicks / pops during sequencing and parameter changes (H-7).
- [ ] Stress with Wi-Fi / WebSocket active (H-8).
- [ ] Audio hot-path memory placement measured (H-7, H-12).

## Configuration

- [x] No Wi-Fi password in git (`tests/test_project_layout.py`).
- [x] GPIOs local / configurable (`config/local.env` -> Kconfig).
- [x] QEMU and hardware build profiles are separate (own build dirs and sdkconfig).
- [x] One-command build / test scripts on Nobara (`scripts/`, `Makefile`).

## MIDI and SLOOP's editor

- [x] MIDI in from the panel (Web MIDI keyboards) plays SLOOP; MIDI out reaches clients that ask
      (host tested, QEMU tested over the WebSocket).
- [x] SLOOP's own web editor works over the WebSocket (`/editor.html`; host + QEMU, headless Chrome).
- [x] USB-MIDI device: the bridge is host tested against SLOOP (`test_usb_midi`: notes, editor
      SysEx framing, fan-out to both transports, a host that stops reading never blocks the web);
      both firmware variants build; the configuration descriptor equals upstream `usb.c`'s.
- [ ] USB-MIDI enumerates and works on the board (H-13) [hw].

## Projects backup

- [x] Backup = SLOOP's store window; checked with SLOOP's own rules (host test `test_store_backup`).
- [x] Restore staged and applied at boot before SLOOP starts; refusals (password, size, other data);
      the API and the device page in Chrome (QEMU tested).
- [ ] On the board (H-15) [hw].

## Firmware update

- [x] Update from the device page: refusals (password, not an image, another project, truncated),
      write + verify + slot switch + restart + self-confirmation, the page in Chrome (QEMU tested).
- [x] Rollback armed (bootloader); a cable flash resets to `ota_0`.
- [ ] Over Wi-Fi on the board, audio effect noted (H-14) [hw].
