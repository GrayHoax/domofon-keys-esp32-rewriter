idf_component_register(
    SRCS "onewire.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio freertos
)
