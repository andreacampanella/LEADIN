/* leds.c - the status LEDs, on boards that have them (g_board.leds)
 *
 * LED 0, network: blinks while connecting or in setup mode, on when the
 *                 network is up, and goes off for a moment on each read.
 * LED 1, audio:   on while a track plays, blinks while paused, else off.
 */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leadin.h"

static volatile int64_t last_read_us;

void leds_note_read(void)
{
    last_read_us = esp_timer_get_time();
}

static void leds_task(void *arg)
{
    int tick = 0;
    /* self-test: all on for a second, then all off, so a wrong polarity shows */
    g_board.leds->set(0xFFFFFFFF);
    vTaskDelay(pdMS_TO_TICKS(1000));
    g_board.leds->set(0);
    vTaskDelay(pdMS_TO_TICKS(500));
    nlog("LEDs: %d, self-test done. LED 1 = network, LED 2 = audio", g_board.leds->count);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        tick++;
        uint32_t on = 0;
        bool blink = (tick / 10) & 1;               /* 1 Hz */
        if (net_is_up()) {
            /* a read in the last 60 ms switches the LED off: it flickers with traffic */
            if (esp_timer_get_time() - last_read_us > 60000)
                on |= 1;
        } else if (blink) {
            on |= 1;
        }
        int state;
        uint32_t pos, start, end;
        audio_status(&state, &pos, &start, &end);
        if (state == 1 || (state == 2 && blink))
            on |= 2;
        g_board.leds->set(on);
    }
}

void leds_start(void)
{
    if (!g_board.leds)
        return;
    xTaskCreatePinnedToCore(leds_task, "leds", 2048, NULL, 1, NULL, 0);
}
