/* hal.h - the hardware layer of the LEADIN firmware
 *
 * Three layers:
 *   core     main/                 protocol, library, audio player, web UI
 *   drivers  components/leadin_hal/ one file for each kind of hardware
 *   boards   boards/<name>/        pins, and which drivers the board uses
 *                                  (audiokit: the Ai-Thinker Audio Kit; qemu: the test board)
 *
 * The core uses only this file and g_board. It has no pins and no board
 * names. The drivers do not include core headers. A board file fills
 * g_board in board_setup(), with drivers from hal_drivers.h.
 *
 * A NULL part means that the board does not have it. The core then works
 * without it: no audio output, no LINE IN mixer, no LEDs.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_netif.h"
#include "sd_protocol_types.h"

/* Log output for the drivers. The core points it at its own log, so the
 * lines also show in the web UI. */
extern void (*hal_log)(const char *fmt, ...);

/* ---- audio output: 44.1 kHz, 16-bit, stereo, interleaved PCM ---- */
typedef struct {
    int line_level;                             /* codec output level, 0..33 */
    int i2s_ws, i2s_dout;                       /* pin overrides from LEADIN.CFG, 0 = board default */
} hal_audio_opts_t;

typedef struct {
    const char *name;                           /* shown in the web UI */
    bool (*start)(const hal_audio_opts_t *opts);
    /* Sends the frames. Blocks until the output can take them, so this
     * call sets the speed of the player. */
    void (*write)(const int16_t *pcm, size_t frames);
    void (*volume)(uint8_t left, uint8_t right);   /* 0..255, 0 = silent */
} hal_audio_t;

/* ---- LINE IN mixer: the DOS PC's sound to the same output ---- */
typedef struct {
    bool (*available)(void);                    /* false if the chip was not found */
    void (*set)(int percent);                   /* 0 = off, 100 = 0 dB */
    /* Selects the input pair that carries LINE IN: 1 or 2, 0 = the board's
     * default. Returns the pair in use. NULL: the mixer has one input. */
    int (*input)(int pair);
} hal_mixer_t;

/* ---- storage: the SD card with the images (FAT32) ---- */
typedef struct {
    const char *name;
    /* Mounts the card at path. Returns ESP_OK and the card, or an error. */
    int (*mount)(const char *path, int max_files, sdmmc_card_t **card);
    /* Checks that the card still answers. Returns ESP_OK if it does. */
    int (*status)(sdmmc_card_t *card);
    void (*unmount)(const char *path, sdmmc_card_t *card);
} hal_storage_t;

/* ---- status LEDs ---- */
typedef struct {
    int count;
    void (*set)(uint32_t on_mask);              /* bit n set = LED n on */
} hal_leds_t;

/* ---- wired network (instead of Wi-Fi) ---- */
typedef struct {
    const char *name;
    /* Starts the interface with DHCP. The core waits for IP_EVENT_ETH_GOT_IP. */
    bool (*start)(const char *hostname, esp_netif_t **netif);
} hal_wired_t;

/* ---- the board ---- */
typedef struct {
    const char *name;
    const hal_storage_t *storage;
    const hal_audio_t   *audio;
    const hal_mixer_t   *mixer;
    const hal_leds_t    *leds;
    const hal_wired_t   *wired;                 /* NULL = the ESP32's Wi-Fi */
} board_t;

extern board_t g_board;

/* In boards/<name>/board.c. The core calls it first, before any other part.
 * It starts shared buses and fills b. */
void board_setup(board_t *b);
