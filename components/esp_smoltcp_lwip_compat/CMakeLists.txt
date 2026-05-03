set(srcs "")
if(CONFIG_LWIP_COMPAT_ENABLE)
    list(APPEND srcs
        "src/fd_table.c"
        "src/sock_wrap.c"
        "src/select_wrap.c"
        "src/netdb_wrap.c"
        "src/esp_netif_shim.c"
        "src/loopback.c"
    )
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        esp_smoltcp
        esp_smoltcp_glue
        esp_event
        esp_timer
        esp_netif
        lwip          # we link against its headers, just not its symbols
    WHOLE_ARCHIVE     # force linker to include all our __wrap_* symbols
)

if(NOT CONFIG_LWIP_COMPAT_ENABLE)
    return()
endif()

# IDF's <lwip/sockets.h> defines `socket()`, `bind()`, etc. as static-inline
# wrappers that call `lwip_socket()`, `lwip_bind()`, ... — so the linker
# only ever sees the `lwip_*` names. Wrapping the bare names does nothing.
set(WRAPPED
    lwip_socket lwip_bind lwip_listen lwip_accept lwip_connect
    lwip_send lwip_recv lwip_sendto lwip_recvfrom lwip_sendmsg lwip_recvmsg
    lwip_select lwip_poll
    lwip_close lwip_fcntl lwip_ioctl lwip_shutdown
    lwip_setsockopt lwip_getsockopt lwip_getsockname lwip_getpeername
    lwip_getaddrinfo lwip_freeaddrinfo lwip_gethostbyname lwip_gethostbyname_r
    lwip_inet_pton lwip_inet_ntop
    lwip_read lwip_write
)
foreach(sym ${WRAPPED})
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${sym}")
endforeach()
