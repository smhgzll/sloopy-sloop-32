idf_component_register(
    SRCS "audio_backend.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2s esp_timer log
)
