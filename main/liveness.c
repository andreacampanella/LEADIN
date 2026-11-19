/* liveness.c - stop music that a DOS client started when that client goes
 * silent (PROTOCOL.md, "Client liveness"). Music started from the web UI
 * is never stopped here.
 */
#include <stdio.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leadin.h"

#define SLOTS 8

static struct {
    uint32_t ip;
    int64_t  last;
} seen[SLOTS];

void liveness_seen(uint32_t ip)
{
    int64_t now = esp_timer_get_time();
    int oldest = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (seen[i].ip == ip) {
            seen[i].last = now;
            return;
        }
        if (seen[i].last < seen[oldest].last)
            oldest = i;
    }
    seen[oldest].ip = ip;
    seen[oldest].last = now;
}

static int64_t last_seen(uint32_t ip)
{
    for (int i = 0; i < SLOTS; i++)
        if (seen[i].ip == ip)
            return seen[i].last;
    return 0;
}

static void task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t owner = audio_owner();
        if (owner == OWNER_NONE || owner == OWNER_WEB || g_set.client_timeout == 0)
            continue;
        int64_t quiet = esp_timer_get_time() - last_seen(owner);
        if (quiet > (int64_t)g_set.client_timeout * 1000000) {
            audio_reset();
            const uint8_t *b = (const uint8_t *)&owner;
            nlog("DOS client %u.%u.%u.%u lost (no message for %u s): music stopped",
                 b[0], b[1], b[2], b[3], g_set.client_timeout);
        }
    }
}

void liveness_start(void)
{
    xTaskCreatePinnedToCore(task, "liveness", 3072, NULL, 3, NULL, 0);
}
