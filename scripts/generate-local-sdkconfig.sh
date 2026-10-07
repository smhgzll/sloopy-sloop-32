#!/usr/bin/env bash
# config/local.env (yours, gitignored) -> config/sdkconfig.local.defaults (generated, gitignored).
# Without config/local.env the defaults below apply (no I2S pins: the firmware runs without audio
# output and says so on the console).
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/common.sh"
if [[ ! -f "$ROOT_DIR/config/local.env" ]]; then
  echo "note: config/local.env not found; using defaults (cp config/local.env.example config/local.env)" >&2
fi
load_local_env

: "${SLOOPY_WIFI_MODE:=AP}"
: "${SLOOPY_WIFI_SSID:=Sloopy-Sloop-32}"
: "${SLOOPY_WIFI_PASSWORD:=change-this-password}"
: "${SLOOPY_HOSTNAME:=sloopy}"
: "${SLOOPY_I2S_BCLK_GPIO:=-1}"
: "${SLOOPY_I2S_WS_GPIO:=-1}"
: "${SLOOPY_I2S_DOUT_GPIO:=-1}"
: "${SLOOPY_USB_MIDI:=0}"

escape_kconfig_string() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

case "$SLOOPY_WIFI_MODE" in AP|STA) ;; *) echo "SLOOPY_WIFI_MODE must be AP or STA" >&2; exit 1 ;; esac
if [[ "$SLOOPY_WIFI_MODE" == AP && ${#SLOOPY_WIFI_PASSWORD} -gt 0 && ${#SLOOPY_WIFI_PASSWORD} -lt 8 ]]; then
  echo "SLOOPY_WIFI_PASSWORD: an AP password needs at least 8 characters (or empty for an open AP)" >&2
  exit 1
fi
for v in SLOOPY_I2S_BCLK_GPIO SLOOPY_I2S_WS_GPIO SLOOPY_I2S_DOUT_GPIO; do
  if [[ ! "${!v}" =~ ^-?[0-9]+$ ]] || (( ${!v} < -1 || ${!v} > 48 )); then
    echo "$v must be a GPIO number 0..48 or -1" >&2
    exit 1
  fi
  g=${!v}
  # N16R16: GPIO 26-32 are the flash / PSRAM bus, 33-37 the octal PSRAM's extra lines
  if (( g >= 26 && g <= 37 )); then
    echo "$v=$g: GPIO 26-37 belong to the flash / octal PSRAM on N16R16 boards; pick another" >&2
    exit 1
  fi
  case "$g" in
    0|3|45|46) echo "warning: $v=$g is a strapping pin (boot mode); a free GPIO is safer (docs/HARDWARE_TESTS.md)" >&2 ;;
    19|20) echo "warning: $v=$g is the native USB port (D-/D+)" >&2 ;;
    43|44) echo "warning: $v=$g is UART0, the serial console" >&2 ;;
    38|48) echo "warning: $v=$g drives the RGB LED on many ESP32-S3 boards" >&2 ;;
  esac
done
if (( SLOOPY_I2S_BCLK_GPIO >= 0 )) && { (( SLOOPY_I2S_BCLK_GPIO == SLOOPY_I2S_WS_GPIO )) ||
   (( SLOOPY_I2S_BCLK_GPIO == SLOOPY_I2S_DOUT_GPIO )); } ||
   { (( SLOOPY_I2S_WS_GPIO >= 0 )) && (( SLOOPY_I2S_WS_GPIO == SLOOPY_I2S_DOUT_GPIO )); }; then
  echo "SLOOPY_I2S_BCLK_GPIO / _WS_GPIO / _DOUT_GPIO must be three different GPIOs" >&2
  exit 1
fi

case "$SLOOPY_USB_MIDI" in
  1|y|yes) usb_midi=y ;;
  0|n|no) usb_midi=n ;;
  *) echo "SLOOPY_USB_MIDI must be 0 or 1" >&2; exit 1 ;;
esac

OUT="$ROOT_DIR/config/sdkconfig.local.defaults"
umask 077
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
cat > "$TMP" <<EOT
# Generated from config/local.env by scripts/generate-local-sdkconfig.sh. DO NOT COMMIT.
CONFIG_SLOOPY_WIFI_MODE="$(escape_kconfig_string "$SLOOPY_WIFI_MODE")"
CONFIG_SLOOPY_WIFI_SSID="$(escape_kconfig_string "$SLOOPY_WIFI_SSID")"
CONFIG_SLOOPY_WIFI_PASSWORD="$(escape_kconfig_string "$SLOOPY_WIFI_PASSWORD")"
CONFIG_SLOOPY_HOSTNAME="$(escape_kconfig_string "$SLOOPY_HOSTNAME")"
CONFIG_SLOOPY_I2S_BCLK_GPIO=$SLOOPY_I2S_BCLK_GPIO
CONFIG_SLOOPY_I2S_WS_GPIO=$SLOOPY_I2S_WS_GPIO
CONFIG_SLOOPY_I2S_DOUT_GPIO=$SLOOPY_I2S_DOUT_GPIO
CONFIG_SLOOPY_USB_MIDI=$usb_midi
EOT
if cmp -s "$TMP" "$OUT"; then
  echo "$OUT is up to date"
else
  cp "$TMP" "$OUT"          # (only on a change: a newer file makes the build regenerate sdkconfig)
  echo "Generated $OUT"
fi
