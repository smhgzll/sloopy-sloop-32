# Port plan

The live milestone list with statuses, commands and results is **PROGRESS.md** (M0–M11); the
next concrete task is in **NEXT_STEPS.md**. This page keeps the plan's shape and the reasoning.

1. **Baseline** (M0–M1): environment, upstream fetched at a locked commit, upstream's tests green,
   `docs/UPSTREAM_MAP.md`.
2. **Port seam** (M2): compile upstream unmodified against a virtual FM-1 HAL; replace only the
   hardware glue (`main.c`, `usb.c`). Host platform first: fast, deterministic tests.
3. **Core on the host** (M3, M6): audio, sequencer, UI and storage through the seam; tests drive
   SLOOP only through the virtual panel, like a user.
4. **ESP32-S3** (M4–M5): the same core as an ESP-IDF component; PSRAM for big buffers; the store
   on a partition; QEMU self-test incl. a render hash equal to the host's.
5. **Panel and transport** (M7–M8): protocol shared by host and ESP32; the FM-1 panel measured
   from a photo; SLOOP's screen transported, not re-implemented.
6. **Network and persistence on the ESP32** (M9): Wi-Fi / AP, embedded panel, WebSocket; the same
   end-to-end test against the host app and the firmware in QEMU.
7. **Hardware** (M10–M11): I2S to the PCM5102A, then the checklist in `docs/HARDWARE_TESTS.md`.

Later (not v1): physical display / buttons / encoders (the virtual HAL is where they plug in),
SD card (a second storage backend), USB-MIDI device, the upstream web editor over WebSocket.
