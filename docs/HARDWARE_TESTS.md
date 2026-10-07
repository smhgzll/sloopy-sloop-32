# Hardware validation

What QEMU and the host cannot prove. Every item is **UNVERIFIED** until it is run on a real
ESP32-S3 N16R16 + PCM5102A and its result is recorded in PROGRESS.md (M10 / M11).

## Wiring (PCM5102A breakout)

| PCM5102A | ESP32-S3 | Suggested GPIO | Note |
| --- | --- | --- | --- |
| VIN / VCC | 5V (board with a regulator, e.g. the purple GY-PCM5102) or 3V3 | | |
| GND | GND | | |
| BCK | `SLOOPY_I2S_BCLK_GPIO` | 4 | |
| LCK (LRCK) | `SLOOPY_I2S_WS_GPIO` | 5 | |
| DIN | `SLOOPY_I2S_DOUT_GPIO` | 6 | |
| SCK | GND | | no MCLK: the PCM5102A makes its clock from BCK |
| FMT | GND | | I2S format |
| XSMT | 3V3 | | soft mute off; left open the DAC can stay muted |
| FLT, DEMP | GND | | normal filter, no de-emphasis |

GPIO 4 / 5 / 6 are free on every ESP32-S3 board (on DevKitC-1 style boards they sit next to the
3V3 / RST pins). On the purple GY-PCM5102 board FLT / DEMP / XSMT / FMT are the solder jumpers
1-4 on the back: bridge **1 L, 2 L, 3 H, 4 L** (they often ship open: no sound), and tie SCK to GND
(the jumper next to the SCK pin, or a wire).

Other pins: pick GPIOs that the board breaks out and that are not used by flash / PSRAM (on
N16R16 octal modules GPIO 26–37: `scripts/generate-local-sdkconfig.sh` refuses them), USB (19, 20),
UART0 (43, 44: the console), strapping pins (0, 3, 45, 46) or the RGB LED many boards have (38 or
48); the script warns about those. Commonly free: 4–18, 21, 39–42. Write them into
`config/local.env`.

## Instruments

- The console prints a line every 10 s: `stats: playing, cpu 34%, render max 2100 us, underruns 0,
  heap int … (min …), stack free a/u …/…`. A half buffer lasts **5805 us**: `render max` must stay
  well below it; `underruns` counts DMA buffers that went out without fresh audio (I2S
  `on_send_q_ovf`).
- `http://<device>/api/status`: the same as JSON (audio frames, underruns, render max, cpu, heap
  minima, stack high-water marks, store errors, panels connected, USB-MIDI link and counts).
- `scripts/probe-pins.sh`: which GPIO a wire reaches, without instruments or firmware. It holds
  the chip in its download mode with every free GPIO as an input (nothing driven), reports pins
  tied high or low, then prints each GPIO that a wire touched to GND pulls low. Use it when there
  is no sound, or with an expansion board / adapter whose labels may not be the ESP32-S3's GPIOs.
- `scripts/qemu-cpu-estimate.sh`: the instruction count of a 4-track song (an estimate to compare
  with `cpu` on the board).

## Checklist

| Id | Test | Pass criteria |
| --- | --- | --- |
| H-1 | `scripts/flash.sh`, `scripts/monitor.sh` | boot report: ESP32-S3, flash 16777216, PSRAM 16777216, `profile hardware`, `I2S TX: 44100 Hz, 32-bit stereo` with your pins, `SLOOP_BOOTED`, `SLOOPY_APP_READY` |
| H-2 | octal PSRAM at 80 MHz, 240 MHz CPU | no `PSRAM` errors at boot; `.pool in PSRAM: 552…` reported; stable for 30 min |
| H-3 | the panel | the address printed by `net:`; panel loads, SLOOP's screen appears, LEDs follow |
| H-4 | Wi-Fi STA and AP | STA joins; a wrong password -> fallback AP named after the hostname (WPA2) after ~8 retries |
| H-5 | audio | a key on track 1 is heard, clean, on both channels, no hum; it sounds like the host app (`scripts/serve-web.sh`) playing the same key with the same preset |
| H-6 | sample rate | at 120 BPM the metronome (GLO -> GLOBAL -> CLICK ON) gives exactly 120 clicks in a minute next to a reference metronome (a 48 kHz clock would run 9 % fast) |
| H-7 | underruns | play the 4-track demo (free take + three tracks) for 10 min: no clicks; `underruns` stays 0 while playing; `cpu` < 70 %, `render max` < 4000 us |
| H-8 | Wi-Fi + audio | the above while two browsers turn knobs and play keys continuously: no dropouts |
| H-9 | flash writes | autosave while stopped (2.5 s idle): silent gap only while stopped, no buzz |
| H-10 | persistence | power off / on: tempo, patterns, presets come back |
| H-11 | latency | key on the panel to sound: note it (expected ~20-40 ms over Wi-Fi + 23 ms DMA) |
| H-12 | stack / heap | after H-7/H-8: no `stack overflow`, internal heap min free > 32 KB (watch the log) |
| H-13 | USB-MIDI (`SLOOPY_USB_MIDI=1`, flash over the UART port) | native USB port to a computer: `usb_midi: a host configured the device`; Linux `amidi -l` lists "Felucca (sloopy-sloop-32)"; `amidi -p hw:N -S '90 3C 64'` plays a note, `'80 3C 00'` ends it; `aseqdump` shows the panel keys' notes; SLOOP's editor in Chrome over Web MIDI (docs/CONFIGURATION.md) connects and reads the parameters; with no program reading, the panel and `/editor.html` stay responsive (`tx_dropped` grows); audio stays clean (H-7 criteria) while MIDI streams |
| H-14 | firmware update over Wi-Fi | device page -> Firmware with a new `build-hw/sloopy_sloop_32.bin`: uploads (note the time), restarts, the page reports the new version from the other slot, `ota_state` `valid`; projects still there; the next update goes back to the first slot; a wrong password / a QEMU build's `.bin` are refused |
| H-15 | projects backup | device page -> Projects backup: download (note the time), change the tempo / record something, wait for the autosave, restore the file: the device restarts with the backed-up project; `store: restored from a backup` on the console; no `store` errors in `/api/status` |

Tuning knobs if H-7 / H-8 fail: `SLOOPY_I2S_DMA_DESC` / `_FRAMES` (more latency, more margin),
moving hot `.pool` buffers to internal RAM (measure first), Wi-Fi power save (already off).

Record results (date, board, pins, outcome) in PROGRESS.md under M10 / M11.
