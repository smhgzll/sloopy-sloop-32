# Tests

Test ladder:

1. `scripts/test-host.sh` — layout checks (`test_project_layout.py`), upstream SLOOP's own suite,
   the port's host tests (`tests/host`: boot, selftest hash, features, soak, protocol, WebSocket
   server, USB-MIDI bridge, store backup) and the web tests (`tests/web`: protocol e2e against the host app, panel
   maths in node, the panel / editor / device page in headless Chrome).
2. `scripts/test-qemu.sh` — the firmware in ESP32-S3 QEMU: self-test (two boots), cross-target
   render hash, the protocol e2e against the firmware's server, then a second boot for the firmware
   backup / restore (`test_store.py`, then the device page in Chrome: `test_browser.py --connect
   --backup`) and the firmware update (`test_ota.py`, then `test_browser.py --connect --ota-image`).
3. Real hardware — `docs/HARDWARE_TESTS.md` (I2S / PCM5102A, real time, Wi-Fi, USB-MIDI, updates).

Do not replace target tests with mocks when the hardware behavior itself is what needs validation.
