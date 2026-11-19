/* hal_drivers.h - the drivers that a board file can use
 *
 * Each driver takes a configuration with the pins, and returns its part
 * for board_t. The configuration must stay valid (make it static const).
 * Each driver has one instance.
 */
#pragma once
#include "driver/gpio.h"
#include "hal.h"

/* ---- audio: I2S to an ES8388 codec, with its LINE IN mixer ---- */
typedef struct {
    int i2s_mclk, i2s_bck, i2s_ws, i2s_dout;
    uint8_t i2c_addr;
    int i2c_pins[4][2];             /* SDA, SCL pairs to try in order; {-1,-1} = end */
    int pa_pin;                     /* speaker amplifier enable, -1 = none */
    bool pa_on;                     /* level for pa_pin */
    uint8_t mixer_input;            /* default LINE IN pair: 0x09 = LIN2/RIN2, 0x00 = LIN1/RIN1 */
} es8388_cfg_t;
const hal_audio_t *audio_es8388(const es8388_cfg_t *cfg);
const hal_mixer_t *mixer_es8388(void);

/* ---- storage: SD card in SDMMC mode ---- */
typedef struct {
    int width;                      /* 1 or 4 data lines */
    int max_khz;                    /* 40000 = high speed; falls back to 20000 */
    bool pullups;                   /* internal pull-ups on CMD and DATA */
} sdmmc_cfg_t;
const hal_storage_t *storage_sdmmc(const sdmmc_cfg_t *cfg);

/* ---- LEDs on GPIO pins ---- */
typedef struct {
    int count;
    int pins[4];
    bool active_low;
} leds_gpio_cfg_t;
const hal_leds_t *leds_gpio(const leds_gpio_cfg_t *cfg);

/* ---- wired: the OpenCores Ethernet that QEMU emulates ---- */
const hal_wired_t *wired_openeth(void);
