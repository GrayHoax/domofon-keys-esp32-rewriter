idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server json ibutton wifi_manager esp_timer esp_app_format status_led
    EMBED_TXTFILES "www/index.html"
)
