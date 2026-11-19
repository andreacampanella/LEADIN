/* sdcard.c - the microSD card (FAT32), through the board's storage driver
 *
 * One recursive mutex guards all file access, because the UDP task, the
 * audio task and the web server all read the card.
 */
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "leadin.h"
#include "sdmmc_cmd.h"

static SemaphoreHandle_t lock;
static sdmmc_card_t *card;
static bool mounted;

void sd_lock(void)
{
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
}

void sd_unlock(void)
{
    xSemaphoreGiveRecursive(lock);
}

bool sd_present(void)
{
    return mounted;
}

static bool do_mount(void)
{
    if (!g_board.storage) {
        nlog("SD card: this board has no card slot");
        return false;
    }
    esp_err_t err = g_board.storage->mount(SD_MOUNT, 8, &card);
    if (err != ESP_OK) {
        nlog("SD card: mount failed (%s)", esp_err_to_name(err));
        return false;
    }
    nlog("SD card: %s, %llu MB, %u kHz", card->cid.name,
         ((unsigned long long)card->csd.capacity) * card->csd.sector_size / (1024 * 1024),
         (unsigned)card->real_freq_khz);
    return true;
}

bool sd_mount(void)
{
    if (!lock)
        lock = xSemaphoreCreateRecursiveMutex();
    sd_lock();
    mounted = do_mount();
    sd_unlock();
    return mounted;
}

/* Checks the card every 2 seconds. A removed card is unmounted (the disc
 * is ejected, so the client reports "no disc"); a new card is mounted. */
static void watch_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (mounted) {
            sd_lock();
            bool ok = g_board.storage->status(card) == ESP_OK;
            if (!ok) {
                nlog("SD card removed");
                disc_eject();
                image_files_flush();
                g_board.storage->unmount(SD_MOUNT, card);
                card = NULL;
                mounted = false;
            }
            sd_unlock();
        } else {
            sd_lock();
            mounted = do_mount();
            sd_unlock();
            if (mounted) {
                library_scan(true);
                if (g_set.disc[0]) {
                    char err[80];
                    if (!disc_select(g_set.disc, err, sizeof(err)))
                        nlog("Last disc not loaded: %s", err);
                }
            }
        }
    }
}

void sd_watch_start(void)
{
    xTaskCreatePinnedToCore(watch_task, "sdwatch", 8192, NULL, 2, NULL, 0);
}
