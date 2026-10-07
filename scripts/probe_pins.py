#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Which ESP32-S3 GPIO does a wire reach? No firmware involved:

    scripts/probe-pins.sh [/dev/ttyACM0] [--seconds 180]

The chip is held in its ROM download mode; every GPIO that is not flash / PSRAM (26-37) or the
serial link (43, 44) becomes an input with its pull-up, and nothing drives a pin. Touch a wire that
goes to the board with GND: the GPIO it lands on reads 0 and is printed. A pin that already reads 0
is tied low (GND, or a pull-down on whatever it is wired to). First, with pull-downs, a pin that
reads 1 is tied high (a supply rail, or a pull-up on the board such as the BOOT button's on
GPIO 0). At the end the chip restarts into its firmware."""
import argparse
import sys
import time

from esptool.cmds import detect_chip

GPIO_BASE, IO_MUX_BASE = 0x60004000, 0x60009000
GPIO_ENABLE_W1TC, GPIO_ENABLE1_W1TC = GPIO_BASE + 0x28, GPIO_BASE + 0x34
GPIO_IN, GPIO_IN1 = GPIO_BASE + 0x3C, GPIO_BASE + 0x40
GPIO_FUNC0_OUT_SEL_CFG = GPIO_BASE + 0x554
FUN_PD, FUN_PU, FUN_IE, FUN_DRV_S, MCU_SEL_S, PIN_FUNC_GPIO = 1 << 7, 1 << 8, 1 << 9, 10, 12, 1
PINS = [*range(0, 22), *range(38, 43), *range(45, 49)]        # not 26-37 (flash / PSRAM), 43/44 (UART0)


def levels(esp):
    lo, hi = esp.read_reg(GPIO_IN), esp.read_reg(GPIO_IN1)
    return {n: (lo >> n) & 1 if n < 32 else (hi >> (n - 32)) & 1 for n in PINS}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default="/dev/ttyACM0")
    ap.add_argument("--seconds", type=float, default=180)
    a = ap.parse_args()
    esp = detect_chip(a.port, connect_mode="default_reset")
    if esp.CHIP_NAME != "ESP32-S3":
        sys.exit(f"{esp.CHIP_NAME}: this probe knows the ESP32-S3's registers only")
    def inputs(pull):
        for n in PINS:
            esp.write_reg(GPIO_FUNC0_OUT_SEL_CFG + 4 * n, 0x100)  # plain GPIO, output enable from GPIO_ENABLE
            if n < 32:
                esp.write_reg(GPIO_ENABLE_W1TC, 1 << n)
            else:
                esp.write_reg(GPIO_ENABLE1_W1TC, 1 << (n - 32))
            esp.write_reg(IO_MUX_BASE + 4 + 4 * n, PIN_FUNC_GPIO << MCU_SEL_S | 2 << FUN_DRV_S | FUN_IE | pull)
        time.sleep(0.05)
        return levels(esp)

    print("probing GPIO " + " ".join(map(str, PINS)), flush=True)
    base = inputs(FUN_PD)
    high0 = [n for n in PINS if base[n]]
    print(f"high with pull-downs (tied to a supply or pulled up): {high0 or 'none'}", flush=True)
    base = inputs(FUN_PU)
    low0 = [n for n in PINS if not base[n]]
    print(f"low with pull-ups (tied to GND or pulled down): {low0 or 'none'}", flush=True)
    print(f"READY: touch the wires with GND now ({a.seconds:.0f} s)", flush=True)
    t0, last, went_low = time.time(), base, {}
    while time.time() - t0 < a.seconds:
        now = levels(esp)
        t = time.time() - t0
        for n in PINS:
            if now[n] != last[n]:
                print(f"  {t:6.1f} s  GPIO {n:2}: {last[n]} -> {now[n]}", flush=True)
                if not now[n]:
                    went_low.setdefault(n, t)
        last = now
        time.sleep(0.03)
    order = sorted(went_low, key=went_low.get)
    print("touched (first time low, in order): " +
          (", ".join(f"GPIO {n} at {went_low[n]:.1f} s" for n in order) if order else "none"), flush=True)
    esp.hard_reset()
    print("restarted into the firmware", flush=True)


if __name__ == "__main__":
    main()
