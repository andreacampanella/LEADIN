/* audio.c - CD audio from the image on the SD card to the board's output
 *
 * Two tasks on core 1:
 *   filler: reads audio sectors from the card into a ring buffer (2 s, PSRAM)
 *   writer: moves the ring buffer to the output; writes silence when it is
 *           empty, so the output always runs and paces the player.
 * The play position comes from the clock (75 sectors per second), not from
 * the bytes written, so it is right also without an output.
 * The output is g_board.audio (hal.h). Without one (no output on the board,
 * or its start failed) the writer only drains the ring buffer at the right
 * speed, so the state machine still works.
 */
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "leadin.h"

#define SECTOR          2352
#define CHUNK_SECTORS   5               /* 1/15 s per card read */
#define WRITE_BYTES     (2 * SECTOR)    /* per output write */

static const hal_audio_t *out;          /* NULL: no output, silent */
static bool started;

static SemaphoreHandle_t lock;
static TaskHandle_t filler;

/* player state, guarded by lock */
static int state;                       /* 0 stopped, 1 playing, 2 paused */
static uint32_t p_start, p_end, p_pos, play_from;
static int64_t t0;
static uint32_t owner = OWNER_NONE;
static uint32_t gen;                    /* changes on every play, pause and stop */
static uint32_t fill_lba;
static uint8_t game_l = 255, game_r = 255;

/* ring buffer, guarded by lock */
static uint8_t *ring;
static size_t ring_size, rd, wr, used;

static void ring_reset(void)
{
    rd = wr = used = 0;
}

static uint32_t position_locked(void)
{
    if (state == 1) {
        uint32_t p = play_from + (uint32_t)((esp_timer_get_time() - t0) * 75 / 1000000);
        return p < p_end ? p : p_end;
    }
    return p_pos;
}

static void begin_locked(uint32_t lba)
{
    gen++;
    ring_reset();
    play_from = lba;
    p_pos = lba;
    fill_lba = lba;
    t0 = esp_timer_get_time();
    state = 1;
}

bool audio_play(uint32_t start, uint32_t count, uint32_t who)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    owner = count ? who : OWNER_NONE;
    if (count == 0) {
        state = 0;
        gen++;
        ring_reset();
        xSemaphoreGive(lock);
        return true;
    }
    bool have_disc = disc_current() != NULL;
    if (!started || !have_disc) {
        state = 0;
        owner = OWNER_NONE;
        xSemaphoreGive(lock);
        return false;
    }
    p_start = start;
    p_end = start + count;
    begin_locked(start);
    xSemaphoreGive(lock);
    xTaskNotifyGive(filler);
    return true;
}

void audio_pause(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (state == 1) {
        uint32_t pos = position_locked();
        state = 2;
        p_start = pos;
        p_pos = pos;
        gen++;
        ring_reset();
    }
    xSemaphoreGive(lock);
}

void audio_stop(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    int s = state;
    xSemaphoreGive(lock);
    if (s == 1) {
        audio_pause();
    } else if (s == 2) {
        xSemaphoreTake(lock, portMAX_DELAY);
        state = 0;
        owner = OWNER_NONE;
        xSemaphoreGive(lock);
    }
}

bool audio_resume(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (state != 2) {
        xSemaphoreGive(lock);
        return false;
    }
    begin_locked(p_start);
    xSemaphoreGive(lock);
    xTaskNotifyGive(filler);
    return true;
}

void audio_reset(void)
{
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    state = 0;
    owner = OWNER_NONE;
    gen++;
    ring_reset();
    xSemaphoreGive(lock);
}

void audio_status(int *s, uint32_t *pos, uint32_t *start, uint32_t *end)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (state == 1 && position_locked() >= p_end) {
        state = 0;
        p_pos = p_end;
        owner = OWNER_NONE;
    }
    *s = state;
    *pos = position_locked();
    *start = p_start;
    *end = p_end;
    xSemaphoreGive(lock);
}

uint32_t audio_owner(void)
{
    int s;
    uint32_t a, b, c;
    audio_status(&s, &a, &b, &c);
    return s ? owner : OWNER_NONE;
}

void audio_set_game_volume(uint8_t left, uint8_t right)
{
    game_l = left;
    game_r = right;
    audio_apply_volume();
}

void audio_get_game_volume(uint8_t *left, uint8_t *right)
{
    *left = game_l;
    *right = game_r;
}

void audio_apply_volume(void)
{
    uint32_t gl = g_set.override ? 255 : game_l;
    uint32_t gr = g_set.override ? 255 : game_r;
    uint8_t l = g_set.mute ? 0 : gl * g_set.volume / 255;
    uint8_t r = g_set.mute ? 0 : gr * g_set.volume / 255;
    if (out)
        out->volume(l, r);
}

const char *audio_output_name(void)
{
    if (!started)
        return NULL;
    return out ? out->name : "none (no audio output found)";
}

/* ---------------------------------------------------------------- tasks */
static void filler_task(void *arg)
{
    const size_t chunk = CHUNK_SECTORS * SECTOR;
    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM);
    if (!buf)
        buf = heap_caps_malloc(chunk, MALLOC_CAP_8BIT);
    for (;;) {
        xSemaphoreTake(lock, portMAX_DELAY);
        bool want = state == 1 && fill_lba < p_end && ring_size - used >= chunk;
        uint32_t my_gen = gen, lba = fill_lba;
        int n = CHUNK_SECTORS;
        if (want && lba + n > p_end)
            n = p_end - lba;
        xSemaphoreGive(lock);
        if (!want) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }
        sd_lock();
        image_t *img = disc_current();
        bool ok = img && my_gen == gen;
        if (ok)
            image_read_audio(img, lba, n, buf);
        sd_unlock();
        if (!ok)
            continue;
        xSemaphoreTake(lock, portMAX_DELAY);
        if (my_gen == gen) {
            size_t bytes = (size_t)n * SECTOR;
            size_t first = ring_size - wr < bytes ? ring_size - wr : bytes;
            memcpy(ring + wr, buf, first);
            memcpy(ring, buf + first, bytes - first);
            wr = (wr + bytes) % ring_size;
            used += bytes;
            fill_lba += n;
        }
        xSemaphoreGive(lock);
    }
}

static void writer_task(void *arg)
{
    static uint8_t buf[WRITE_BYTES] __attribute__((aligned(4)));
    for (;;) {
        size_t n = 0;
        xSemaphoreTake(lock, portMAX_DELAY);
        if (used) {
            n = used < WRITE_BYTES ? used : WRITE_BYTES;
            size_t first = ring_size - rd < n ? ring_size - rd : n;
            memcpy(buf, ring + rd, first);
            memcpy(buf + first, ring, n - first);
            rd = (rd + n) % ring_size;
            used -= n;
        }
        xSemaphoreGive(lock);
        if (n < WRITE_BYTES)
            memset(buf + n, 0, WRITE_BYTES - n);
        if (n)
            xTaskNotifyGive(filler);
        if (out) {
            out->write((const int16_t *)buf, WRITE_BYTES / 4);
        } else {
            vTaskDelay(pdMS_TO_TICKS(WRITE_BYTES * 1000 / 176400));
        }
    }
}

void audio_init(void)
{
    lock = xSemaphoreCreateMutex();
    ring_size = 2 * 75 * SECTOR;        /* 2 seconds */
    ring = heap_caps_malloc(ring_size, MALLOC_CAP_SPIRAM);
    if (!ring) {
        ring_size = 12 * SECTOR;        /* no PSRAM (QEMU): 160 ms */
        ring = heap_caps_malloc(ring_size, MALLOC_CAP_8BIT);
    }
    if (!ring) {
        nlog("Audio: no memory for the buffer, CD audio is off");
        return;
    }
    if (g_board.audio) {
        hal_audio_opts_t o = {.line_level = g_cfg.line_level, .i2s_ws = g_cfg.i2s_ws, .i2s_dout = g_cfg.i2s_dout};
        if (g_board.audio->start(&o))
            out = g_board.audio;
        else
            nlog("Audio: %s did not start, CD audio plays silently", g_board.audio->name);
    }
    started = true;
    audio_apply_volume();
    xTaskCreatePinnedToCore(filler_task, "audio_fill", 4096, NULL, 5, &filler, 1);
    xTaskCreatePinnedToCore(writer_task, "audio_out", 3072, NULL, 10, NULL, 1);
    nlog("Audio: %s, buffer %u ms", audio_output_name(),
         (unsigned)(ring_size * 1000 / 176400));
}
