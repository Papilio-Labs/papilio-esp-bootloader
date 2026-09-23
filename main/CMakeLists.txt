idf_component_register(SRCS
    "main.c"
    "jtag_gowin.c"
    "wifi_init.c"
    "http_server.c"
    "serial_flash.c"
    "wifi_log.c"
    INCLUDE_DIRS "."
)

# wifi_log.c intercepts printf()/ESP_LOGx() output via __wrap__write_r (Phase 4).
target_link_options(${COMPONENT_LIB} INTERFACE -Wl,--wrap=_write_r)

