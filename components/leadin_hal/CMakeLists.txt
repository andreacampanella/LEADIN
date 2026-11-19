# leadin_hal - the drivers. All of them are built. The linker drops the
# drivers that the selected board does not use.
idf_component_register(
    SRCS "hal_common.c" "audio_es8388.c" "storage_sd.c" "leds_gpio.c" "wired_openeth.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif sdmmc esp_driver_gpio
    PRIV_REQUIRES esp_driver_i2s esp_driver_i2c esp_driver_sdmmc fatfs vfs esp_eth)
