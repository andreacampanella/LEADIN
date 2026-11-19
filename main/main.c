/* main.c - LEADIN: a CD-ROM server for DOS PCs, on an ESP32 board
 *
 * The board (boards/<name>/board.c) gives the hardware. See hal.h.
 *
 * Start-up order:
 *   1. log, board, NVS settings
 *   2. SD card, LEADIN.CFG
 *   3. audio output, mixer, LEDs
 *   4. network (Wi-Fi or the setup access point, or the board's wired network)
 *   5. library, last disc, UDP protocol, liveness, web UI
 */
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leadin.h"
#include "nvs_flash.h"

board_t g_board;

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_SW:        return "restart";
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "brownout (power supply too weak)";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default:                return "other";
    }
}

static void heap_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        nlog("Free memory: %u KB internal, %u KB PSRAM",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}

void app_main(void)
{
    nlog_init();
    board_setup(&g_board);
    nlog("LEADIN firmware %s on %s, reset reason: %s, %s", LEADIN_VERSION, g_board.name, reset_reason(), ota_running());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();

    if (!sd_mount())
        nlog("No SD card: insert a FAT32 card with LEADIN.CFG and the images");
    config_load_file();

    image_init();
    audio_init();
    mixer_apply();
    leds_start();                       /* boards with status LEDs */

    net_start();
    if (net_setup_mode()) {
        sd_watch_start();
        return;                         /* only the setup page runs */
    }

    library_scan(true);
    if (g_set.disc[0]) {
        char e[100];
        if (!disc_select(g_set.disc, e, sizeof(e)))
            nlog("Last disc not loaded: %s", e);
    }
    leadin_udp_start();
    liveness_start();
    webui_start();
    sd_watch_start();
    xTaskCreatePinnedToCore(heap_task, "heap", 2560, NULL, 1, NULL, 0);
    /* Everything started: this image is good. Confirm it now, before the
     * network is up, so that an early reset does not roll it back. */
    ota_mark_valid();
}
