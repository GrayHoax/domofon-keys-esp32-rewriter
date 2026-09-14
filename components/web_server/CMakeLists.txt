idf_component_register(
    SRCS "web_server.c" "web_keys.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_driver_gpio ibutton activekey keydb wifi_manager esp_timer esp_app_format status_led
    EMBED_TXTFILES "www/index.html"
)
