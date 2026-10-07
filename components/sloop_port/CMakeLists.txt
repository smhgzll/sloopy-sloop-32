idf_component_register(
    SRCS "platform_esp32.c" "sloop_port.c"
    INCLUDE_DIRS "include"
    REQUIRES sloop_core esp_partition esp_timer
    PRIV_REQUIRES log esp_system
)
