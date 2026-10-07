/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: the USB-MIDI device on the ESP32-S3's native USB port (see usb_midi.h).
 *
 * The same device as the FM-1's (upstream usb.c): one class-compliant USB-MIDI interface (one
 * jack each way, 64-byte bulk endpoints), the pid.codes test VID / PID upstream uses, and a
 * product name with "Felucca" in it, which is how SLOOP's web editor finds its port. TinyUSB runs
 * on core 0 (core 1 is SLOOP's), and so does the task here that polls both directions every
 * millisecond through the bridge.
 *
 * The native USB PHY is then TinyUSB's: the USB-Serial/JTAG console and flashing through that
 * port stop while this runs (docs/CONFIGURATION.md), hence opt-in (SLOOPY_USB_MIDI).
 */
#include "usb_midi.h"

#include "sdkconfig.h"

#if CONFIG_SLOOPY_USB_MIDI
#include <stdio.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usb_midi";

enum { ITF_MIDI_CONTROL, ITF_MIDI_STREAMING, ITF_COUNT };
#define EP_MIDI_OUT 0x01
#define EP_MIDI_IN 0x81
#define CFG_LEN (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)

static const tusb_desc_device_t s_dev = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,                           /* (per interface, as usb.c) */
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = CONFIG_SLOOPY_USB_VID,
    .idProduct = CONFIG_SLOOPY_USB_PID,
    .bcdDevice = 0x0300,                            /* (usb.c: 3.00) */
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const uint8_t s_cfg[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CFG_LEN, 0, 100),
    TUD_MIDI_DESCRIPTOR(ITF_MIDI_CONTROL, 0, EP_MIDI_OUT, EP_MIDI_IN, 64),
};

static char s_serial[13];                           /* the MAC, so two boards are two devices */
static const char *s_str[] = {
    (const char[]){0x09, 0x04},                     /* English (US) */
    "sloopy-sloop-32",
    CONFIG_SLOOPY_USB_PRODUCT,
    s_serial,
};

static EXT_RAM_BSS_ATTR usb_midi_bridge_t s_br;   /* (PSRAM: SysEx buffers, polled once a millisecond) */
static volatile int s_started, s_linked;
static usb_midi_counts_t s_counts;                  /* (a copy for usb_midi_get_stats) */

static int link_write(void *ctx, const uint8_t pkt[4])
{
    (void)ctx;
    return tud_midi_packet_write(pkt) ? 1 : 0;
}

static void usb_midi_task(void *arg)
{
    (void)arg;
    for (;;) {
        int up = tud_mounted() && !tud_suspended() && tud_midi_mounted();
        if (up != s_br.up) {
            usb_midi_bridge_link(&s_br, up);
            ESP_LOGI(TAG, "%s", up ? "a host configured the device: SLOOP's MIDI is on USB" : "USB host gone");
        }
        if (up) {
            uint8_t pkt[4];
            int i;
            for (i = 0; i < 64 && tud_midi_packet_read(pkt); i++)   /* (a flood still lets output through) */
                usb_midi_bridge_rx(&s_br, pkt);
            usb_midi_bridge_tx(&s_br, (uint32_t)(esp_timer_get_time() / 1000), link_write, NULL);
        }
        s_linked = s_br.up;
        s_counts = s_br.n;
        vTaskDelay(up ? 1 : pdMS_TO_TICKS(20));     /* (every millisecond while a host is there) */
    }
}

int usb_midi_start(void)
{
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    uint8_t mac[6] = {0};
    esp_err_t err;
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof s_serial, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cfg.task.xCoreID = 0;                           /* core 1 is SLOOP's audio + UI */
    cfg.descriptor.device = &s_dev;
    cfg.descriptor.full_speed_config = s_cfg;
    cfg.descriptor.string = s_str;
    cfg.descriptor.string_count = sizeof s_str / sizeof s_str[0];
    usb_midi_bridge_init(&s_br);
    err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB: %s", esp_err_to_name(err));
        return -1;
    }
    if (xTaskCreatePinnedToCore(usb_midi_task, "usb_midi", 4096, NULL, 5, NULL, 0) != pdPASS)
        return -1;
    s_started = 1;
    ESP_LOGI(TAG, "USB-MIDI device \"%s\" (%04x:%04x) on the native USB port", CONFIG_SLOOPY_USB_PRODUCT,
             CONFIG_SLOOPY_USB_VID, CONFIG_SLOOPY_USB_PID);
    return 0;
}

void usb_midi_get_stats(usb_midi_stats_t *st)
{
    st->enabled = s_started;
    st->linked = s_linked;
    st->n = s_counts;
}

#else /* !CONFIG_SLOOPY_USB_MIDI */

int usb_midi_start(void) { return -1; }

void usb_midi_get_stats(usb_midi_stats_t *st)
{
    usb_midi_stats_t z = {0};
    *st = z;
}

#endif
