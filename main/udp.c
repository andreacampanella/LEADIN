/* udp.c - the LEADIN protocol (PROTOCOL.md, version 2) and the current
 * disc. One function per message type. The counters for the web UI are in
 * stats.c.
 */
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "leadin.h"

enum {
    T_DISCOVER = 0x01, T_OFFER = 0x81, T_READ = 0x02, T_DATA = 0x82,
    T_PLAY = 0x03, T_STOP = 0x04, T_RESUME = 0x05, T_ASTATUS = 0x06,
    T_VOLUME = 0x07, T_MEDIA = 0x08, T_HEARTBEAT = 0x09, T_LIST = 0x0A,
    T_SELECT = 0x0B, T_ANNOUNCE = 0x0C,
};
enum { ST_OK = 0, ST_ERROR = 1, ST_CHANGED = 2, ST_NODISC = 3 };

#define PART_SIZE       1024
#define MAX_PER_READ    8
#define CACHE_SECTORS   64          /* 128 KB per slot */
#define LIST_PAGE       20

static int sock = -1;
static image_t *current;
static uint8_t cur_disc_id = 1;

/* Read-ahead: a pool of slots of CACHE_SECTORS each, in PSRAM, as many as
 * the memory allows (MAX_SLOTS at most, PSRAM_KEEP left free). A READ is
 * served from a slot when it can. When a slot is used, the block after it
 * is read into the least recently used slot by the prefetch task, on the
 * other core, while the reply goes out. So on a sequential read the card
 * work hides behind the send, and a game that jumps between a few files,
 * or a video player that reads the index at the end and then streams from
 * the start, finds its blocks still in RAM. The slots are touched under
 * sd_lock only. */
#define MAX_SLOTS   16
#define PSRAM_KEEP  (1536 * 1024)   /* for uploads, the web UI and the rest */
typedef struct {
    uint8_t *buf;
    uint32_t lba, count;        /* count 0 = empty */
    uint32_t used;              /* stamp of the last use, for the LRU choice */
    uint8_t disc;
    bool filling;               /* the prefetch task is writing it: not a victim */
} slot_t;
static slot_t slots[MAX_SLOTS];
static int nslots;
static uint32_t use_stamp;
static uint32_t prefetch_lba;   /* what the prefetch task should read next */
static uint8_t prefetch_disc;
static TaskHandle_t prefetch_task_h;

/* ----------------------------------------------------------------- disc */
image_t *disc_current(void)
{
    return current;
}

uint8_t disc_id(void)
{
    return cur_disc_id;
}

static void swap_disc(image_t *img)
{
    sd_lock();
    audio_reset();
    image_t *old = current;
    current = img;
    cur_disc_id = cur_disc_id % 255 + 1;
    for (int i = 0; i < nslots; i++)
        slots[i].count = 0;
    if (old)
        image_close(old);
    sd_unlock();
}

bool disc_select(const char *rel_path, char *err, size_t errlen)
{
    if (library_find(rel_path) < 0) {
        library_scan(true);
        if (library_find(rel_path) < 0) {
            snprintf(err, errlen, "not in the library: %s", rel_path);
            return false;
        }
    }
    image_t *img = image_open(rel_path, err, errlen);
    if (!img)
        return false;
    swap_disc(img);
    strlcpy(g_set.disc, rel_path, sizeof(g_set.disc));
    settings_save();
    stats_event("Disc changed: %s", rel_path);
    if (img->warnings[0])
        stats_event("Warning: %s", img->warnings);
    return true;
}

void disc_eject(void)
{
    bool had = current != NULL;
    swap_disc(NULL);
    if (had) {
        g_set.disc[0] = 0;
        settings_save();
        stats_event("Disc removed");
    }
}

/* ------------------------------------------------------------- protocol */
static void reply(const struct sockaddr_in *to, uint8_t type, uint8_t status,
                  const uint8_t *seq, const uint8_t *body, size_t n, uint8_t *buf)
{
    memcpy(buf, "NCD\x01", 4);
    buf[4] = type;
    buf[5] = status;
    buf[6] = seq[0];
    buf[7] = seq[1];
    if (n && body != buf + 8)
        memcpy(buf + 8, body, n);
    sendto(sock, buf, 8 + n, 0, (const struct sockaddr *)to, sizeof(*to));
}

static int64_t last_read_us;

int64_t leadin_last_read_us(void)
{
    return last_read_us;
}

static slot_t *slot_with(uint32_t lba, int count)
{
    for (int i = 0; i < nslots; i++)
        if (slots[i].count && slots[i].disc == cur_disc_id &&
            lba >= slots[i].lba && lba + count <= slots[i].lba + slots[i].count)
            return &slots[i];
    return NULL;
}

/* The slot to fill next: an empty one, else the least recently used. */
static slot_t *slot_victim(void)
{
    slot_t *v = NULL;
    for (int i = 0; i < nslots; i++) {
        if (slots[i].filling)
            continue;
        if (!slots[i].count || slots[i].disc != cur_disc_id)
            return &slots[i];
        if (!v || slots[i].used < v->used)
            v = &slots[i];
    }
    return v;
}

/* Fills a slot with up to CACHE_SECTORS from lba. Caller holds sd_lock. */
static bool fill_slot(slot_t *sl, uint32_t lba)
{
    sl->count = 0;
    if (!current)
        return false;
    const track_t *t = image_track_at(current, lba);
    if (!t || t->audio)
        return false;
    uint32_t n = CACHE_SECTORS;
    if (lba + n > t->start + t->length)
        n = t->start + t->length - lba;
    if (!n || image_read(current, lba, n, sl->buf) != 0)
        return false;
    sl->lba = lba;
    sl->count = n;
    sl->disc = cur_disc_id;
    return true;
}

/* Asks the prefetch task for the block that follows a slot, into the other slot. */
static void prefetch_after(slot_t *sl)
{
    uint32_t next = sl->lba + sl->count;
    if (slot_with(next, 1))
        return;                     /* already there */
    prefetch_lba = next;
    prefetch_disc = cur_disc_id;
    if (prefetch_task_h)
        xTaskNotifyGive(prefetch_task_h);
}

static void prefetch_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Fill in pieces, the lock released between them, so a READ that
         * arrives meanwhile waits for one piece, not for the whole block. */
        sd_lock();
        uint32_t lba = prefetch_lba;
        uint8_t disc = prefetch_disc;
        slot_t *sl = NULL;
        uint32_t n = 0;
        const track_t *t = current ? image_track_at(current, lba) : NULL;
        if (disc == cur_disc_id && t && !t->audio && !slot_with(lba, 1) && (sl = slot_victim())) {
            n = CACHE_SECTORS;
            if (lba + n > t->start + t->length)
                n = t->start + t->length - lba;
            sl->count = 0;
            sl->filling = true;
        }
        sd_unlock();
        if (!sl || !n)
            continue;
        bool ok = true;
        for (uint32_t i = 0; i < n && ok; i += 8) {
            uint32_t m = n - i < 8 ? n - i : 8;
            sd_lock();
            ok = disc == cur_disc_id && current && image_read(current, lba + i, m, sl->buf + i * 2048) == 0;
            sd_unlock();
        }
        sd_lock();
        if (ok && disc == cur_disc_id) {
            sl->lba = lba;
            sl->count = n;
            sl->disc = disc;
            sl->used = ++use_stamp;
        }
        sl->filling = false;
        sd_unlock();
    }
}

/* Reads count sectors at lba from a slot, or from the card. Caller holds sd_lock. */
static bool cached_read(uint32_t lba, int count, uint8_t *out)
{
    slot_t *sl = slot_with(lba, count);
    if (!sl) {
        if (!nslots)
            return image_read(current, lba, count, out) == 0;
        sl = slot_victim();             /* a miss: fill a slot from lba */
        if (!sl || !fill_slot(sl, lba) || sl->count < (uint32_t)count)
            return image_read(current, lba, count, out) == 0;
    }
    sl->used = ++use_stamp;
    memcpy(out, sl->buf + (lba - sl->lba) * 2048, count * 2048);
    prefetch_after(sl);
    return true;
}

static void handle(uint8_t *pkt, int len, const struct sockaddr_in *from, uint8_t *tx)
{
    static uint8_t data[MAX_PER_READ * 2048];
    if (len < 8 || memcmp(pkt, "NCD\x01", 4))
        return;
    uint8_t type = pkt[4];
    const uint8_t *seq = pkt + 6;
    uint32_t ip = from->sin_addr.s_addr;
    char ips[16];
    ip_str(ip, ips);
    liveness_seen(ip);

    switch (type) {
    case T_DISCOVER: {
        stats_seen(ip);
        stats_event("DISCOVER from %s", ips);
        sd_lock();
        size_t n = current ? image_offer_body(current, cur_disc_id, tx + 8)
                           : empty_offer_body(cur_disc_id, tx + 8);
        sd_unlock();
        reply(from, T_OFFER, ST_OK, seq, tx + 8, n, tx);
        break;
    }
    case T_READ: {
        if (len < 13)
            return;
        uint32_t lba;
        memcpy(&lba, pkt + 8, 4);
        int count = pkt[12];
        uint16_t mask = 0xFFFF;
        if (len >= 15)
            memcpy(&mask, pkt + 13, 2);
        int status = ST_OK;
        sd_lock();
        if (!current)
            status = ST_NODISC;
        else if (len >= 16 && pkt[15] != cur_disc_id)
            status = ST_CHANGED;
        else if (count < 1 || count > MAX_PER_READ) {
            status = ST_ERROR;
        } else {
            if (!cached_read(lba, count, data))
                status = ST_ERROR;
        }
        sd_unlock();
        uint16_t sq;
        memcpy(&sq, seq, 2);
        leds_note_read();
        stats_read(ip, sq, count * 2048, status != ST_OK);
        if (status != ST_OK) {
            static const uint8_t err_body[4] = {0xFF, 0, 0, 0};
            reply(from, T_DATA, status, seq, err_body, 4, tx);
            return;
        }
        int parts = count * 2;
        for (int p = 0; p < parts; p++) {
            if (!(mask & (1u << p)))
                continue;
            tx[8] = p;
            tx[9] = parts;
            tx[10] = tx[11] = 0;
            memcpy(tx + 12, data + p * PART_SIZE, PART_SIZE);
            reply(from, T_DATA, ST_OK, seq, tx + 8, 4 + PART_SIZE, tx);
        }
        last_read_us = esp_timer_get_time();
        break;
    }
    case T_PLAY: {
        if (len < 16)
            return;
        uint32_t start, count;
        memcpy(&start, pkt + 8, 4);
        memcpy(&count, pkt + 12, 4);
        bool ok = audio_play(start, count, ip);
        int tn = -1;
        sd_lock();
        if (current) {
            const track_t *t = image_track_at(current, start);
            if (t)
                tn = t->number;
        }
        sd_unlock();
        stats_event("PLAY from %s: track %d, %lu s%s", ips, tn, (unsigned long)(count / 75),
                    ok ? "" : " (audio is off)");
        reply(from, T_PLAY | 0x80, ok ? ST_OK : ST_ERROR, seq, NULL, 0, tx);
        break;
    }
    case T_STOP:
        audio_stop();
        stats_event("STOP from %s", ips);
        reply(from, T_STOP | 0x80, ST_OK, seq, NULL, 0, tx);
        break;
    case T_RESUME: {
        bool ok = audio_resume();
        stats_event("RESUME from %s%s", ips, ok ? "" : " (not paused)");
        reply(from, T_RESUME | 0x80, ok ? ST_OK : ST_ERROR, seq, NULL, 0, tx);
        break;
    }
    case T_ASTATUS: {
        int state;
        uint32_t pos, start, end;
        audio_status(&state, &pos, &start, &end);
        uint8_t b[14] = {(uint8_t)state, 0};
        memcpy(b + 2, &pos, 4);
        memcpy(b + 6, &start, 4);
        memcpy(b + 10, &end, 4);
        reply(from, T_ASTATUS | 0x80, ST_OK, seq, b, sizeof(b), tx);
        break;
    }
    case T_VOLUME:
        if (len < 10)
            return;
        audio_set_game_volume(pkt[8], pkt[9]);
        stats_event("Game volume from %s: %u/%u", ips, pkt[8], pkt[9]);
        reply(from, T_VOLUME | 0x80, ST_OK, seq, NULL, 0, tx);
        break;
    case T_MEDIA: {
        stats_seen(ip);
        uint8_t b[2] = {cur_disc_id, current ? 1 : 0};
        reply(from, T_MEDIA | 0x80, ST_OK, seq, b, 2, tx);
        break;
    }
    case T_HEARTBEAT:
        stats_seen(ip);
        reply(from, T_HEARTBEAT | 0x80, ST_OK, seq, NULL, 0, tx);
        break;
    case T_LIST: {
        uint16_t start = 0;
        if (len >= 10)
            memcpy(&start, pkt + 8, 2);
        int total = library_scan(false);
        int cur = 0xFFFF;
        sd_lock();
        if (current) {
            int i = library_find(current->path);
            if (i >= 0)
                cur = i;
        }
        sd_unlock();
        uint8_t *b = tx + 8;
        uint16_t t16 = total, c16 = cur;
        memcpy(b, &t16, 2);
        memcpy(b + 2, &start, 2);
        memcpy(b + 4, &c16, 2);
        int n = 0;
        size_t off = 7;
        char path[256];
        for (int i = start; i < total && n < LIST_PAGE; i++, n++) {
            if (!library_get(i, path, sizeof(path), NULL))
                break;
            size_t l = strlen(path);
            if (l > 63)
                l = 63;
            b[off++] = l;
            for (size_t k = 0; k < l; k++)
                b[off++] = (path[k] & 0x80) ? '?' : path[k];
        }
        b[6] = n;
        reply(from, T_LIST | 0x80, ST_OK, seq, b, off, tx);
        break;
    }
    case T_SELECT: {
        if (len < 10)
            return;
        uint16_t index;
        memcpy(&index, pkt + 8, 2);
        int status = ST_ERROR;
        char path[256], err[100];
        if (index == 0xFFFF) {
            disc_eject();
            status = ST_OK;
        } else if (library_scan(false) > index && library_get(index, path, sizeof(path), NULL)) {
            if (disc_select(path, err, sizeof(err))) {
                status = ST_OK;
                stats_event("SELECT from %s: %s", ips, path);
            } else {
                stats_event("SELECT from %s failed: %s", ips, err);
            }
        }
        reply(from, T_SELECT | 0x80, status, seq, NULL, 0, tx);
        break;
    }
    default:
        break;
    }
}

static void udp_task(void *arg)
{
    static uint8_t rx[1500], tx[1100];
    for (;;) {
        while (!net_is_up())
            vTaskDelay(pdMS_TO_TICKS(500));
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int on = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(LEADIN_UDP_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            nlog("UDP: bind failed, errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        nlog("LEADIN protocol on UDP port %d", LEADIN_UDP_PORT);
        for (;;) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &fl);
            if (n < 0) {
                nlog("UDP: receive error %d, reopening", errno);
                break;
            }
            handle(rx, n, &from, tx);
        }
        close(sock);
        sock = -1;
    }
}

/* ANNOUNCE every second, as a broadcast. A Wi-Fi access point often does
 * not pass broadcasts from the cable to its wireless clients, so a DOS PC's
 * DISCOVER never arrives. Broadcasts in the other direction do arrive. The
 * client learns the server's addresses from ANNOUNCE and sends DISCOVER
 * directly. */
static void announce_task(void *arg)
{
    static const uint8_t pkt[8] = {'N', 'C', 'D', 1, T_ANNOUNCE, 0, 0, 0};
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(LEADIN_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    int s = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (sock < 0 || !net_is_up()) {
            if (s >= 0)
                close(s);
            s = -1;
            continue;
        }
        if (s < 0) {
            /* its own socket, so that port 2352 only carries requests and replies */
            s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            int on = 1;
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
        }
        if (sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            close(s);
            s = -1;
        }
    }
}

void leadin_udp_start(void)
{
    stats_start();
    xTaskCreatePinnedToCore(announce_task, "announce", 2048, NULL, 2, NULL, 0);
    /* the read-ahead pool: what PSRAM allows, PSRAM_KEEP left free */
    size_t have = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int want = have > PSRAM_KEEP ? (int)((have - PSRAM_KEEP) / (CACHE_SECTORS * 2048)) : 0;
    if (want > MAX_SLOTS)
        want = MAX_SLOTS;
    for (nslots = 0; nslots < want; nslots++) {
        slots[nslots].buf = heap_caps_malloc(CACHE_SECTORS * 2048, MALLOC_CAP_SPIRAM);
        if (!slots[nslots].buf)
            break;
    }
    if (nslots >= 2)
        xTaskCreatePinnedToCore(prefetch_task, "prefetch", 4096, NULL, 5, &prefetch_task_h, 1);
    nlog("Read-ahead: %d blocks of %d KB%s", nslots, CACHE_SECTORS * 2,
         nslots ? "" : " (no PSRAM: every request reads the card)");
    xTaskCreatePinnedToCore(udp_task, "udp", 6144, NULL, 6, NULL, 0);
}
