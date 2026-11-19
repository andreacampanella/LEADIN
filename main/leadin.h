/* leadin.h - interfaces between the parts of the LEADIN firmware */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "hal.h"                /* the board: g_board */

#define LEADIN_VERSION "1.9"
#define PROTOCOL_VERSION    2

/* Paths and ports */
#define SD_MOUNT            "/sdcard"
#define CFG_FILE            SD_MOUNT "/LEADIN.CFG"
#define LEADIN_UDP_PORT     2352
#define HTTP_PORT           80
#define SETUP_AP_SSID       "LEADIN-SETUP"

/* Defaults (also documented in the README) */
#define DEFAULT_HOSTNAME        "leadin"
#define DEFAULT_LINE_LEVEL      30      /* codec output level, 0..33, 30 = 0 dB */
#define DEFAULT_CLIENT_TIMEOUT  15      /* seconds */
#define DEFAULT_PC_VOLUME       80      /* percent */
#define WIFI_CONNECT_TIMEOUT_S  60

/* ---- log.c: ring buffer of the last lines, also sent to the console ---- */
void nlog_init(void);
void nlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- config.c: LEADIN.CFG on the SD card, settings in NVS ---- */
typedef struct {
    char ssid[33];
    char password[65];
    char hostname[32];
    int  line_level;            /* 0..33 */
    bool have_wifi;             /* SSID found in LEADIN.CFG */
    int  i2s_ws, i2s_dout;      /* I2S_WS, I2S_DOUT: pin overrides for codec boards, 0 = board default */
} leadin_cfg_t;

typedef struct {
    uint8_t  volume;            /* web volume, 0..255 */
    bool     mute;
    bool     override;          /* ignore the game volume */
    uint16_t client_timeout;    /* seconds, 0 = never */
    uint8_t  pc_volume;         /* LINE IN level, 0..100 */
    bool     pc_mute;
    uint8_t  pc_input;          /* codec input pair for LINE IN: 1 or 2, 0 = the board's default */
    bool     ota_enabled;       /* the web UI may replace the firmware (off at every start) */
    bool     light_theme;       /* the web UI in light colors (kept in NVS) */
    char     disc[256];         /* last disc, path relative to the SD root */
} settings_t;

extern leadin_cfg_t g_cfg;
extern settings_t  g_set;
void config_load_file(void);
bool config_write_wifi(const char *ssid, const char *password);
void settings_load(void);
void settings_save(void);

/* ---- network.c: Wi-Fi, or the board's wired network ---- */
void net_start(void);
bool net_is_up(void);
bool net_setup_mode(void);      /* the setup access point is running */
const char *net_ip(void);
int  net_rssi(void);

/* ---- sdcard.c ---- */
bool sd_mount(void);
bool sd_present(void);
void sd_lock(void);             /* one lock for all file access (recursive) */
void sd_unlock(void);
void sd_watch_start(void);      /* remounts after a card is removed and inserted */

/* ---- library.c: images on the SD card, sorted by path, case ignored ---- */
typedef struct {
    char    *path;              /* relative to the SD root, with '/' */
    uint32_t kbytes;
} lib_entry_t;

int  library_scan(bool force);  /* returns the number of entries */
int64_t leadin_last_read_us(void);   /* when the last READ from DOS was served, 0 = never */
bool library_get(int index, char *path, size_t len, uint32_t *kbytes);
int  library_find(const char *path);

/* ---- image.c: ISO and BIN/CUE ---- */
#define MAX_TRACKS  99
#define MAX_FILES   99

typedef struct {
    uint8_t  number;
    bool     audio;
    uint8_t  mode;              /* index into the mode table in image.c */
    uint16_t sector_size;
    uint8_t  skip;              /* header bytes before the 2048 user bytes */
    uint8_t  file;              /* index into image_t.files */
    uint32_t start;             /* absolute LBA */
    uint32_t length;            /* sectors */
    uint32_t file_offset;       /* byte offset of INDEX 01 in its file */
} track_t;

typedef struct {
    char     path[256];         /* relative to the SD root */
    char    *files[MAX_FILES];  /* full paths */
    int      nfiles;
    track_t  tracks[MAX_TRACKS];
    int      ntracks;
    uint32_t sectors;
    char     warnings[160];
} image_t;

image_t *image_open(const char *rel_path, char *err, size_t errlen);
void image_close(image_t *img);
void image_init(void);           /* the card-read buffer, at start */
int  image_read(image_t *img, uint32_t lba, int count, uint8_t *out);       /* 0 = ok */
int  image_read_audio(image_t *img, uint32_t lba, int count, uint8_t *out); /* 2352 per sector */
const track_t *image_track_at(const image_t *img, uint32_t lba);
const char *image_mode_name(const track_t *t);
size_t image_offer_body(const image_t *img, uint8_t disc_id, uint8_t *out);
size_t empty_offer_body(uint8_t disc_id, uint8_t *out);
void image_files_flush(void);   /* close cached file handles */

/* ---- udp.c: the protocol and the current disc ---- */
void leadin_udp_start(void);
image_t *disc_current(void);    /* call with sd_lock held */
uint8_t disc_id(void);
bool disc_select(const char *rel_path, char *err, size_t errlen);
void disc_eject(void);

/* ---- stats.c: counters for the web UI and the screen ---- */
void stats_start(void);
void ip_str(uint32_t ip, char *out);               /* "a.b.c.d", out needs 16 bytes */
void stats_seen(uint32_t ip);                       /* a client sent a request */
void stats_read(uint32_t ip, uint16_t seq, uint32_t bytes, bool error);
void stats_json(cJSON *obj);
void stats_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- audio.c: CD audio from the image to the board's audio output ---- */
#define OWNER_NONE  0u
#define OWNER_WEB   1u          /* any other value is the client's IPv4 address */

void audio_init(void);
bool audio_play(uint32_t start, uint32_t count, uint32_t owner);
void audio_stop(void);          /* MSCDEX STOP: pause, or clear the pause */
void audio_pause(void);
bool audio_resume(void);
void audio_reset(void);         /* stop completely */
void audio_status(int *state, uint32_t *pos, uint32_t *start, uint32_t *end);
uint32_t audio_owner(void);
void audio_set_game_volume(uint8_t left, uint8_t right);
void audio_get_game_volume(uint8_t *left, uint8_t *right);
void audio_apply_volume(void);  /* after a change of g_set volume, mute or override */
const char *audio_output_name(void);    /* NULL when audio is off */

/* ---- mixer.c: LINE IN (the DOS PC's sound), if the board has a mixer ---- */
void mixer_apply(void);         /* after a change of g_set pc_volume, pc_mute or pc_input */
bool mixer_available(void);
int  mixer_input(void);         /* the input pair in use, 0 = no choice */

/* ---- liveness.c: stop music when the DOS client that started it is gone ---- */
void liveness_seen(uint32_t ip);
void liveness_start(void);

/* ---- leds.c: status LEDs, if the board has them ---- */
void leds_start(void);
void leds_note_read(void);      /* a READ arrived: flicker the network LED */

/* ---- webui.c and ota.c ---- */
void webui_start(void);
void setup_portal_start(void);
void ota_mark_valid(void);
const char *ota_running(void);      /* "image ota_1 (pending verify)" */
struct httpd_req;
int  ota_upload_handler(struct httpd_req *req);   /* 403 while ota_enabled is off */

