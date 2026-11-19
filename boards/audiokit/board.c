/* Ai-Thinker ESP32 Audio Kit V2.2 (ES8388 codec)
 *
 * LEDs: D4 shows the network (blinks while connecting, on when up, off
 * briefly on each read), D5 the audio (on while playing, blinks when paused).
 * CD audio on the EARPHONES jack, the DOS PC's sound from LINE IN mixed in.
 * microSD in 1-bit SDMMC mode: set the DIP switches for CMD and DATA3 to ON.
 * The codec I2C pins differ between batches, so the driver tries each pair.
 */
#include "hal_drivers.h"

static const es8388_cfg_t codec = {
    .i2s_mclk = 0,              /* MCLK must be GPIO0, 1 or 3 on the ESP32 */
    .i2s_bck = 27,
    .i2s_ws = 25,               /* Some batches have WS and data the other way round (26 and 25). */
    .i2s_dout = 26,             /* LEADIN.CFG: I2S_WS=26 and I2S_DOUT=25 swaps them without a build. */
    .i2c_addr = 0x10,
    .i2c_pins = {{33, 32}, {18, 23}, {-1, -1}},
    .pa_pin = 21,               /* NS4150 speaker amplifier: off, the jack is used */
    .pa_on = false,
    .mixer_input = 0x09,        /* LINE IN jack on LIN2 / RIN2. The web UI can select LIN1 / RIN1. */
};

static const leds_gpio_cfg_t leds = {
    .count = 2,
    .pins = {22, 19},           /* D4, D5: next to the headphone jack */
    .active_low = true,
};

static const sdmmc_cfg_t sd = {
    .width = 1,                 /* slot 1: CLK 14, CMD 15, D0 2 */
    .max_khz = 40000,
    .pullups = true,
};

void board_setup(board_t *b)
{
    b->name = "ESP32 Audio Kit";
    b->storage = storage_sdmmc(&sd);
    b->audio = audio_es8388(&codec);
    b->mixer = mixer_es8388();
    b->leds = leds_gpio(&leds);
}
