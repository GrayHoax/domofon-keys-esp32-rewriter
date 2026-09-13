idf_component_register(
    SRCS "keydb.c"
    INCLUDE_DIRS "include"
    REQUIRES ibutton
    PRIV_REQUIRES nvs_flash
)
