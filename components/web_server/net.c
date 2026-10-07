/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: the network (see web_server.h). The radio and the lwIP task stay on core 0;
 * SLOOP's audio and UI own core 1. */
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

#include "web_server.h"

#if CONFIG_ETH_USE_OPENETH
#include "esp_eth.h"
#else
#include "esp_wifi.h"
#endif
#if CONFIG_SLOOPY_MDNS
#include "mdns.h"
#endif

static const char *TAG = "net";
static sloop_net_settings_t s_cfg;               /* the settings in effect (sloop_settings_load) */
static EventGroupHandle_t s_ev;
#define EV_GOT_IP BIT0
static char s_ip[20];
static esp_netif_t *s_netif;

static void announce(void)                      /* mDNS: http://<hostname>.local/ */
{
#if CONFIG_SLOOPY_MDNS
    static int started;
    if (started)
        return;
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s_cfg.hostname);
        mdns_instance_name_set("SLOOP (sloopy-sloop-32)");
        mdns_service_add("SLOOP panel", "_http", "_tcp", CONFIG_SLOOPY_WEB_PORT, NULL, 0);
        started = 1;
        ESP_LOGI(TAG, "mDNS: http://%s.local/", s_cfg.hostname);
    }
#endif
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = data;
    (void)arg; (void)base; (void)id;
    snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "IP %s: the panel is at http://%s/", s_ip, s_ip);
    announce();
    xEventGroupSetBits(s_ev, EV_GOT_IP);
}

#if CONFIG_ETH_USE_OPENETH
/* QEMU: OpenCores Ethernet MAC + the PHY QEMU models, DHCP from QEMU's user-mode network */
static esp_err_t net_eth(void)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_eth_handle_t eth = NULL;
    esp_eth_mac_t *mac;
    esp_eth_phy_t *phy;
    esp_eth_config_t eth_cfg;
    s_netif = esp_netif_new(&cfg);
    esp_netif_set_hostname(s_netif, s_cfg.hostname);
    phy_cfg.autonego_timeout_ms = 100;
    mac = esp_eth_mac_new_openeth(&mac_cfg);
    phy = esp_eth_phy_new_dp83848(&phy_cfg);
    eth_cfg = (esp_eth_config_t)ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL));
    ESP_LOGI(TAG, "Ethernet (QEMU OpenCores MAC)");
    return esp_eth_start(eth);
}
#else
/* into a fixed-size Wi-Fi config field (32-byte SSID, 64-byte password): a full-length value needs
 * no terminator there; the config struct is zeroed beforehand */
static void field_copy(uint8_t *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    memcpy(dst, src, n < cap ? n : cap);
}

static int s_sta_retries;
static uint8_t s_ap_fallback;

static void start_ap(void);

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++s_sta_retries <= 8) {
            ESP_LOGW(TAG, "Wi-Fi \"%s\": not connected, retry %d", s_cfg.ssid, s_sta_retries);
            esp_wifi_connect();
        } else if (!s_ap_fallback) {
            ESP_LOGW(TAG, "Wi-Fi \"%s\" unreachable: starting the device's own access point", s_cfg.ssid);
            s_ap_fallback = 1;
            start_ap();
        }
    } else if (id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "access point \"%s\" up: the panel is at http://192.168.4.1/", s_ap_fallback ? s_cfg.hostname : s_cfg.ssid);
        snprintf(s_ip, sizeof s_ip, "192.168.4.1");
        announce();
        xEventGroupSetBits(s_ev, EV_GOT_IP);
    }
}

static void start_ap(void)
{
    wifi_config_t ap = {0};
    const char *ssid = s_ap_fallback ? s_cfg.hostname : s_cfg.ssid;
    const char *pass = s_cfg.password;   /* (the fallback AP too: never an open network) */
    esp_netif_t *apif = esp_netif_create_default_wifi_ap();
    (void)apif;
    field_copy(ap.ap.ssid, sizeof ap.ap.ssid, ssid);
    ap.ap.ssid_len = (uint8_t)(strlen(ssid) < sizeof ap.ap.ssid ? strlen(ssid) : sizeof ap.ap.ssid);
    field_copy(ap.ap.password, sizeof ap.ap.password - 1, pass);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = strlen(pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (s_ap_fallback) {
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_AP);
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (s_ap_fallback)
        esp_wifi_start();
}

static esp_err_t net_wifi(void)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    int sta = !strcmp(s_cfg.mode, "STA");
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));    /* the credentials come from the build, not NVS */
    if (sta) {
        wifi_config_t c = {0};
        s_netif = esp_netif_create_default_wifi_sta();
        esp_netif_set_hostname(s_netif, s_cfg.hostname);
        field_copy(c.sta.ssid, sizeof c.sta.ssid, s_cfg.ssid);
        field_copy(c.sta.password, sizeof c.sta.password - 1, s_cfg.password);
        c.sta.threshold.authmode = strlen(s_cfg.password) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &c));
        ESP_LOGI(TAG, "Wi-Fi: joining \"%s\" as \"%s\"", s_cfg.ssid, s_cfg.hostname);
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        start_ap();
        ESP_LOGI(TAG, "Wi-Fi: access point \"%s\"", s_cfg.ssid);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);    /* power save adds tens of ms of latency to every control */
    return ESP_OK;
}
#endif

esp_err_t sloop_net_start(void)
{
    sloop_settings_load(&s_cfg);
    ESP_LOGI(TAG, "settings: %s \"%s\", hostname %s (%s)", s_cfg.mode, s_cfg.ssid, s_cfg.hostname,
             s_cfg.from_nvs ? "changed on the device" : "from the build");
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_ETH_USE_OPENETH
    return net_eth();
#else
    return net_wifi();
#endif
}

const char *sloop_net_wait_ip(uint32_t timeout_ms)
{
    if (s_ev)
        xEventGroupWaitBits(s_ev, EV_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return s_ip;
}
