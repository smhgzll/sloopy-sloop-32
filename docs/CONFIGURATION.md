# Configuration

## Your board: `config/local.env` (gitignored)

```bash
cp config/local.env.example config/local.env
$EDITOR config/local.env
```

| Variable | Meaning |
| --- | --- |
| `SLOOPY_I2S_BCLK_GPIO` | PCM5102A BCK |
| `SLOOPY_I2S_WS_GPIO` | PCM5102A LCK (LRCLK / WS) |
| `SLOOPY_I2S_DOUT_GPIO` | PCM5102A DIN |
| `SLOOPY_WIFI_MODE` | `STA` (join your network) or `AP` (the device's own network) |
| `SLOOPY_WIFI_SSID`, `SLOOPY_WIFI_PASSWORD` | the network to join (STA) or to create (AP, WPA2 when >= 8 characters) |
| `SLOOPY_HOSTNAME` | DHCP hostname; also the SSID of the fallback AP |
| `SLOOPY_USB_MIDI` | `1`: SLOOP is a USB-MIDI device on the native USB port (see below); default `0` |
| `SLOOPY_SERIAL_PORT` | default port for `flash.sh` / `monitor.sh` |

`-1` pins: the firmware runs without audio output and says so on the console. In STA mode, after 8
failed connection attempts the device starts its own access point (SSID = hostname, same password)
so the panel stays reachable at `http://192.168.4.1/`.

`scripts/build-hardware.sh` runs `scripts/generate-local-sdkconfig.sh`, which validates the values
and writes `config/sdkconfig.local.defaults` (gitignored, written only when it changes).

## USB-MIDI device (`SLOOPY_USB_MIDI=1`)

The board's native USB port (GPIO19/20, often labelled "USB" next to a "COM" / "UART" port)
becomes SLOOP's USB-MIDI interface, as the FM-1's USB-C port is: notes from a DAW or a keyboard
host play SLOOP, the keys' notes go out (on the selected track's channel), and SLOOP's own web
editor finds the "Felucca (sloopy-sloop-32)" port over Web MIDI in Chrome / Edge, as it finds an
FM-1: upstream's hosted copy (https://isod89.github.io/sloop-fm1/webapp/editor/, which follows
upstream's latest release, not necessarily the locked commit), or `upstream/sloop-fm1/web/` served
from `http://localhost` (Web MIDI needs a secure context). The editor of the locked commit is
always available over the network at `http://<device>/editor.html` (WebSocket, no USB needed).

TinyUSB then owns that port: **its USB-Serial/JTAG console and flashing stop while the firmware
runs**. Flash and monitor through the board's UART port, or put the chip in download mode (hold
BOOT, tap RESET or plug in) to flash over native USB. Hence off by default: turn it on after the
first bring-up (`docs/HARDWARE_TESTS.md` H-13).

Kconfig: `SLOOPY_USB_VID` / `SLOOPY_USB_PID` (0x1209 / 0x0001, the pid.codes test ID upstream
uses), `SLOOPY_USB_PRODUCT` (must contain "Felucca" for the editor; ASCII). The serial number is
the chip's MAC. `GET /api/status` -> `usb_midi`: `enabled`, `linked` (a host configured it), packet
counts, `rx_dropped` (SLOOP's inbox was full), `tx_dropped` (the host was not reading).

## Updating the firmware from the browser

`http://<device>/device.html` -> **Firmware**: pick `build-hw/sloopy_sloop_32.bin` (from
`scripts/build-hardware.sh`), enter the current Wi-Fi password, **Update and restart**. The device
checks that it is an ESP32-S3 image of this project, writes it into the other OTA slot (`ota_0` /
`ota_1`), verifies it, restarts into it, and the page reports the new version. Audio pauses while
flash is written. Projects, presets, user samples (the `sloop` partition) and NVS settings stay.

The bootloader's rollback is on (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`): a new image boots
"pending" and keeps itself only when SLOOP runs and the web server is up (so it can be updated
again); if it fails or resets before that, the bootloader starts the previous image again.
`scripts/flash.sh` (USB / serial) always writes `ota_0` and resets the OTA state, so a cable flash
wins over earlier updates. API: `POST /api/ota` (docs/PROTOCOL.md).

## Projects backup

`http://<device>/device.html` -> **Projects backup**: **Download a backup** saves everything SLOOP
has saved (its settings, the 4 projects and the working one, the user preset banks, the 3 user
sample slots: the `sloop` partition, 448 KiB) as `sloop-store-<hostname>.bin`. It holds what SLOOP
has *saved*: SLOOP autosaves the working project when nothing sounds, 2.5 s after the last touch,
at most every 20 s.

**Restore** (with the current Wi-Fi password) checks the file with SLOOP's own rules (storage.c's
headers and CRCs, the sample slots' headers), stages it in the `stage` partition and restarts; the
boot copies it into `sloop` before SLOOP starts (no race with SLOOP's own writes), checking it
again. A cut-off copy is redone at the next boot. API: `GET` / `POST /api/store`
(docs/PROTOCOL.md). A file from the host app's store (`build-host/sloop-store.bin`) is the same
format.

## MIDI keyboard in the browser (panel "midi in")

The panel's **midi in** lets a MIDI keyboard on the computer / phone play SLOOP (channels 1-3 the
synth tracks, 10 the drums, as SLOOP's MIDI in). Browsers offer Web MIDI only to `https://` or
`localhost` pages, and the board serves plain `http://`: open the panel through
`scripts/panel-localhost.sh [device]` at `http://localhost:8080/` (it forwards the port, WebSocket
included), or in Chrome list the board's address under
`chrome://flags/#unsafely-treat-insecure-origin-as-secure`.

Without a browser (Linux): `scripts/midi-bridge.py [--device sloopy.local] [--port 24:0] [--follow]` reads the
keyboard's ALSA port with `aseqdump` and sends its channel messages over the same WebSocket (every
hardware MIDI input when no port is given; `--follow`: the keyboard plays the track selected on the
panel, as the panel's keys do, instead of channel 1 -> track 1). Keep the panel's midi in off
meanwhile.

## Changing the network without rebuilding

`http://<device>/device.html` (linked from the panel as **device**): live status and the network
settings. A change needs the current Wi-Fi password, is kept in NVS (namespace `sloopy`) over the
build's values, and applies after a restart; **Back to the build's settings** erases it. API:
`GET /api/settings` (never returns the password), `POST /api/settings`
`{mode, ssid, password, hostname, current_password, restart}`, `POST /api/settings/reset`
`{current_password}`.

## Build profiles

| Profile | Build dir | sdkconfig layers |
| --- | --- | --- |
| QEMU | `build-qemu` | `sdkconfig.defaults` + `config/sdkconfig.qemu.defaults` |
| hardware | `build-hw` | `sdkconfig.defaults` + `config/sdkconfig.hardware.defaults` + `config/sdkconfig.local.defaults` |

Each build dir has its own `sdkconfig`, regenerated when its layers change (a hash is kept next to
it), so keep lasting settings in those files rather than in menuconfig. The QEMU profile differs
from hardware in: quad PSRAM (KNOWN_ISSUES KI-5), 160 MHz, null audio, OpenCores Ethernet instead of
Wi-Fi, the boot self-test.

Project options (`main/Kconfig.projbuild`, menu "Sloopy Sloop 32"): the pins and Wi-Fi above,
`SLOOPY_ENABLE_WEB`, `SLOOPY_WEB_PORT` (80), `SLOOPY_MDNS`, `SLOOPY_USB_MIDI` (+ `_VID`, `_PID`,
`_PRODUCT`), `SLOOPY_AUDIO_SAMPLE_RATE` (44100: SLOOP's tables and tempo clock assume it),
`SLOOPY_I2S_DMA_DESC` / `SLOOPY_I2S_DMA_FRAMES` (4 x 256), `SLOOPY_SLOOP_CORE` (1), `SLOOPY_SELFTEST`.
Managed components (`main/idf_component.yml`, pinned in `dependencies.lock`): `espressif/mdns`,
`espressif/esp_tinyusb` (with `espressif/tinyusb`); both profiles compile them, only the hardware
profile starts them.

## Flash layout (`config/partitions.csv`, 16 MB)

| Partition | Offset | Size | Use |
| --- | --- | --- | --- |
| nvs | 0x9000 | 24 KB | ESP-IDF |
| otadata, phy_init | 0xF000 | | |
| ota_0, ota_1 | 0x20000, 0x420000 | 4 MB each | firmware (OTA-ready layout) |
| sloop | 0x820000 | 448 KB | SLOOP's store: its FM-1 flash map 0x90000..0xFFFFF |
| userfs | 0x890000 | 6.7 MB | reserved (sample library later) |
| stage | 0xF40000 | 512 KB | a projects backup being restored (copied into `sloop` at the next boot) |
| coredump | 0xFC0000 | 256 KB | crash dumps |

## Host app

`build-host/sloop_host --help`: port, bind address (`--bind 0.0.0.0` for other devices on the
LAN), web root, store file (default `build-host/sloop-store.bin`: the host's "flash"), audio
(`alsa` / `null`), ALSA device, WAV recording.
