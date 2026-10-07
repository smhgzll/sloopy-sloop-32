# Hardware — v1

## Required

- ESP32-S3 N16R16-class development board
- PCM5102A DAC module
- USB data/power cable
- stereo line output connection from PCM5102A

## Not required in v1

- physical LCD/OLED/TFT
- physical buttons
- encoders/knobs
- keybed/pads
- SD card

## PCM5102A connection

The exact GPIOs are intentionally **not fixed in source control** because the user's ESP32-S3 board is a clone/variant and pin exposure may differ.

Configure before real-hardware build:

- BCLK
- LRCLK / WS
- DATA OUT

PCM5102A generally only needs the I2S clocks/data for this use; whether a specific breakout exposes/configures additional pins depends on the module.

Wiring table, safe GPIO choices and the validation checklist: [HARDWARE_TESTS.md](HARDWARE_TESTS.md).

## An ESP32-S3 module in a classic ESP32 (WROOM-32) adapter

ESP32-S3-WROOM-1 / -2 modules have the WROOM-32's outline, so they fit the adapters and programming
boards made for it, and GND, 3V3, EN, TX0 and RX0 line up (the module powers up and flashes). The
other labels are the WROOM-32's GPIOs, not the S3's. Measured on the first board (`scripts/probe-pins.sh`
on the left of each pair) and following from the two pinouts:

| Adapter label | ESP32-S3 GPIO | | Adapter label | ESP32-S3 GPIO |
| --- | --- | --- | --- | --- |
| SVP (36) | 4 | | IO0 | 0 (boot strap) |
| SVN (39) | 5 | | IO4 | 35 (octal PSRAM: do not use) |
| IO34 | **6** (measured) | | IO16 | none (measured; IO36, PSRAM) |
| IO35 | **7** (measured) | | IO17 | none (measured; IO37, PSRAM) |
| IO32 | 15 | | IO5 | 38 |
| IO33 | **16** (measured) | | IO18 | **39** (measured) |
| IO25 | 17 | | IO19 | 40 |
| IO26 | 18 | | (NC) | 41 |
| IO27 | 8 | | IO21 | 42 |
| IO14 | 19 (USB D-) | | IO22 | 2 |
| IO12 | 20 (USB D+) | | IO23 | 1 |

A GND contact of the bottom row can land on GPIO 46 (it reads low). Check the pins you use with
`scripts/probe-pins.sh` (touch each terminal with GND) and write the **S3** GPIO numbers into
`config/local.env`.

## Board validation before hardware milestone

Verify on the actual board:

- chip reports ESP32-S3
- flash reports 16 MB
- PSRAM reports 16 MB
- PSRAM mode/config boots reliably
- chosen GPIOs are broken out and not reserved by flash/PSRAM/USB/onboard hardware
- native USB wiring if later required
- 3.3 V rail remains stable under Wi-Fi load

## Local configuration

```bash
cp config/local.env.example config/local.env
$EDITOR config/local.env
./scripts/generate-local-sdkconfig.sh
```

Never commit `config/local.env` or `config/sdkconfig.local.defaults`.
