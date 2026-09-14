idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_event ibutton activekey keydb wifi_manager captive_dns web_server status_led
)
