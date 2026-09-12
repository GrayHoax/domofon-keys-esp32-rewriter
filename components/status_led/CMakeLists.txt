idf_component_register(
    SRCS "status_led.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_timer
)
