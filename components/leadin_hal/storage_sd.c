/* storage_sd.c - the SD card in SDMMC mode */
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "hal_drivers.h"
#include "sdmmc_cmd.h"

static int plain_status(sdmmc_card_t *card)
{
    return sdmmc_get_status(card);
}

static void plain_unmount(const char *path, sdmmc_card_t *card)
{
    esp_vfs_fat_sdcard_unmount(path, card);
}

/* ---------------------------------------------------------------- SDMMC */
static const sdmmc_cfg_t *mmc;

static int mmc_mount(const char *path, int max_files, sdmmc_card_t **card)
{
    esp_vfs_fat_sdmmc_mount_config_t mc = {.max_files = max_files};
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = mmc->max_khz;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = mmc->width;
    if (mmc->pullups)
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(path, &host, &slot, &mc, card);
    if ((err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE) && host.max_freq_khz > SDMMC_FREQ_DEFAULT) {
        /* a fast card in high speed mode can fail on long wires: try 20 MHz */
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        err = esp_vfs_fat_sdmmc_mount(path, &host, &slot, &mc, card);
    }
    return err;
}

static const hal_storage_t mmc_part = {
    .name = "SDMMC",
    .mount = mmc_mount,
    .status = plain_status,
    .unmount = plain_unmount,
};

const hal_storage_t *storage_sdmmc(const sdmmc_cfg_t *c)
{
    mmc = c;
    return &mmc_part;
}
