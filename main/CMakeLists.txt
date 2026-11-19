# The core. It uses the drivers in components/leadin_hal through hal.h, and
# the board file boards/<CONFIG_LEADIN_BOARD>/board.c.
set(board_c "${CMAKE_CURRENT_LIST_DIR}/../boards/${CONFIG_LEADIN_BOARD}/board.c")
if(CONFIG_LEADIN_BOARD AND NOT EXISTS "${board_c}")
    message(FATAL_ERROR "LEADIN: no board file ${board_c}. Set CONFIG_LEADIN_BOARD to a folder in boards/.")
endif()
if(NOT CONFIG_LEADIN_BOARD)
    set(board_c "")             # first CMake pass, before sdkconfig is read
endif()

idf_component_register(
    SRCS "main.c" "log.c" "config.c" "network.c" "sdcard.c" "library.c" "image.c"
         "udp.c" "stats.c" "audio.c" "mixer.c" "liveness.c" "webui.c" "ota.c"
         "leds.c" ${board_c}
    INCLUDE_DIRS "."
    EMBED_TXTFILES "webui.html"
    REQUIRES leadin_hal esp_wifi esp_netif esp_eth esp_event nvs_flash fatfs sdmmc vfs
             esp_http_server esp_timer app_update json mdns lwip esp_psram)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
