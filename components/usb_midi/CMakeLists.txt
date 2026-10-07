# SPDX-License-Identifier: GPL-3.0-only
# SLOOP as a USB-MIDI device (usb_midi.h): the portable bridge (also built and tested on the host,
# host/CMakeLists.txt) and the TinyUSB device, compiled in when CONFIG_SLOOPY_USB_MIDI is set.
idf_component_register(
    SRCS "usb_midi_bridge.c" "usb_midi.c"
    INCLUDE_DIRS "include"
    REQUIRES sloop_core
    PRIV_REQUIRES espressif__esp_tinyusb esp_timer esp_hw_support log
)
