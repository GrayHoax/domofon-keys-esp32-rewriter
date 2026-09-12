idf_component_register(
    SRCS "ibutton.c"
    INCLUDE_DIRS "include"
    REQUIRES onewire
    PRIV_REQUIRES esp_timer
)
