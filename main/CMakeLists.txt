idf_component_register(
    SRCS "app_main.c" "selftest.c"
    INCLUDE_DIRS "."
    REQUIRES sloop_core sloop_port audio_backend web_server usb_midi
    PRIV_REQUIRES esp_system spi_flash heap esp_psram nvs_flash esp_timer log app_update esp_app_format
)
