# vseismo - two-channel geophone digitizer on ESP32-C5, ESP-IDF v6.1

idf_component_register(
    SRCS
        "vseismo.c"
        "web_server.c"
        "captive_dns.c"
        "event_log.c"
        "fault.c"
        "console.c"
        "ds323x.c"
        "ads131m0x.c"
        "acq.c"
        "timebase.c"
    INCLUDE_DIRS
        "."
    EMBED_FILES
        "www/index.html"
        "www/portal.html"
    REQUIRES
        esp_wifi
        esp_http_server
        esp_event
        esp_netif
        esp_timer
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        esp_driver_ledc
        esp_driver_mcpwm
        esp_driver_rmt
        esp_hw_support
        nvs_flash
        lwip
        mbedtls
        app_update
        esp_app_format
        console
)

# Build time as a Unix epoch. SNTP times older than this (minus a day) are
# rejected as implausible. Evaluated at CMake configure time, which is close
# enough: the guard only has to catch 1970/2000 garbage.
string(TIMESTAMP SEISMO_BUILD_EPOCH "%s" UTC)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SEISMO_BUILD_EPOCH=${SEISMO_BUILD_EPOCH}LL)
