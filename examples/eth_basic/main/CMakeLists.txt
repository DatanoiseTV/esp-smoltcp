idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_smoltcp esp_smoltcp_lwip_compat esp_eth esp_event nvs_flash
)
