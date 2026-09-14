idf_component_register(
    SRCS "ibutton.c"
    INCLUDE_DIRS "include"
    REQUIRES onewire activekey esp_driver_gpio
    PRIV_REQUIRES esp_timer
)
