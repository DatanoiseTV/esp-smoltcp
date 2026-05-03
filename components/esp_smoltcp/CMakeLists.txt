set(srcs
    "src/esp_smoltcp.c"
    "src/esp_smoltcp_sock.c"
)

set(reqs
    esp_smoltcp_glue
    esp_eth           # apps hand us an esp_eth_handle_t in attach_eth
    esp_event
    esp_timer
    esp_netif         # for esp_mac.h / MAC byte helpers only
    esp_common
    esp_hw_support    # esp_random / esp_read_mac
    esp_driver_gpio   # IDF v5+ split — was just "driver"
    log
    nvs_flash
)

if(CONFIG_NETSTACK_NTP_CLIENT)
    list(APPEND srcs "src/ntp.c")
endif()

if(CONFIG_NETSTACK_PTP)
    list(APPEND srcs "src/ptp.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES ${reqs}
)
