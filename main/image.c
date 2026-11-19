/* image.c - ISO, BIN/CUE and MDF/MDS images: the track list, the Red Book
 * addresses, and sector reads for data and audio tracks.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "leadin.h"

typedef struct {
    const char *name;
    uint16_t size;
    int16_t skip;   /* -1 = audio */
} mode_t_;

static const mode_t_ modes[] = {
    {"MODE1/2048", 2048, 0},
    {"MODE1/2352", 2352, 16},
    {"MODE2/2336", 2336, 8},
    {"MODE2/2352", 2352, 24},
    {"AUDIO", 2352, -1},
};
#define NMODES (sizeof(modes) / sizeof(modes[0]))

const char *image_mode_name(const track_t *t)
{
    return modes[t->mode].name;
}

/* ---- a small cache of open files (the audio task and the UDP task each
 * read one file at a time, so four handles are plenty) ---- */
#define FD_SLOTS 4
static struct {
    const char *path;   /* points into an image_t, compared by value */
    char key[260];
    int fd;
    uint32_t used;
} slots[FD_SLOTS];
static uint32_t use_clock;

void image_files_flush(void)
{
    sd_lock();
    for (int i = 0; i < FD_SLOTS; i++)
        if (slots[i].key[0]) {
            close(slots[i].fd);
            slots[i].key[0] = 0;
        }
    sd_unlock();
}

static int file_fd(const char *path)
{
    int lru = 0;
    for (int i = 0; i < FD_SLOTS; i++) {
        if (slots[i].key[0] && !strcmp(slots[i].key, path)) {
            slots[i].used = ++use_clock;
            return slots[i].fd;
        }
        if (!slots[i].key[0] || slots[i].used < slots[lru].used)
            lru = i;
    }
    if (slots[lru].key[0])
        close(slots[lru].fd);
    slots[lru].key[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    strlcpy(slots[lru].key, path, sizeof(slots[lru].key));
    slots[lru].fd = fd;
    slots[lru].used = ++use_clock;
    return fd;
}

/* Every card read goes through this buffer in internal RAM. The SD driver
 * can DMA only into internal RAM: given a PSRAM buffer, it falls back to one
 * 512-byte block per command through a tiny bounce buffer, which is about
 * five times slower. The image buffers live in PSRAM, so we bounce here in
 * large pieces instead and copy. */
static uint8_t *bounce;
static size_t bounce_len;
static uint8_t *raw;                    /* up to 8 raw sectors (BIN/CUE, MDF) */

/* Takes the card-read buffers at start, before anything else can take the
 * memory: the DMA buffer (32 KB, or what internal DMA memory allows) and the
 * buffer for raw 2352/2448-byte sectors. */
void image_init(void)
{
    for (bounce_len = 32 * 1024; bounce_len >= 4096; bounce_len /= 2) {
        bounce = heap_caps_malloc(bounce_len, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (bounce)
            break;
    }
    raw = heap_caps_malloc(8 * 2448, MALLOC_CAP_SPIRAM);
    if (!raw)   /* no PSRAM: ask for internal RAM by name, malloc() would not */
        raw = heap_caps_malloc(8 * 2448, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!bounce || !raw)
        nlog("No memory for the card-read buffers: reads will fail");
    else if (bounce_len < 32 * 1024)
        nlog("Card-read buffer is %u KB (short of internal memory)", (unsigned)(bounce_len / 1024));
}

static int read_at(const char *path, uint32_t offset, uint8_t *out, size_t n)
{
    if (!bounce)
        return -1;
    int fd = file_fd(path);
    if (fd < 0)
        return -1;
    if (lseek(fd, offset, SEEK_SET) < 0)
        return -1;
    size_t got = 0;
    while (got < n) {
        size_t want = n - got;
        if (want > bounce_len)
            want = bounce_len;
        ssize_t r = read(fd, bounce, want);
        if (r <= 0)
            break;
        memcpy(out + got, bounce, r);
        got += r;
    }
    return (int)got;
}

/* ---- parsing ---- */
static int msf_to_frames(const char *s)
{
    int m = 0, sec = 0, f = 0;
    if (sscanf(s, "%d:%d:%d", &m, &sec, &f) != 3)
        return -1;
    return (m * 60 + sec) * 75 + f;
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static char *dup_path(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!p)
        p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static void warn(image_t *img, const char *w)
{
    if (strstr(img->warnings, w))
        return;
    size_t n = strlen(img->warnings);
    snprintf(img->warnings + n, sizeof(img->warnings) - n, "%s%s", n ? "; " : "", w);
}

static bool load_iso(image_t *img, const char *full, char *err, size_t errlen)
{
    long size = file_size(full);
    if (size < 2048 * 16) {
        snprintf(err, errlen, "not an ISO image (too small)");
        return false;
    }
    img->files[0] = dup_path(full);
    img->nfiles = 1;
    track_t *t = &img->tracks[0];
    memset(t, 0, sizeof(*t));
    t->number = 1;
    t->mode = 0;
    t->sector_size = 2048;
    t->length = size / 2048;
    img->ntracks = 1;
    return true;
}

static bool load_cue(image_t *img, const char *full, char *err, size_t errlen)
{
    char dir[256];
    strlcpy(dir, full, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;

    FILE *f = fopen(full, "r");
    if (!f) {
        snprintf(err, errlen, "cannot open the .cue file");
        return false;
    }
    /* first_track[i] = index of the first track of file i */
    int first_track[MAX_FILES];
    int index1[MAX_TRACKS];
    char line[320];
    bool ok = true;
    while (ok && fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char)*p))
            p++;
        if (!*p)
            continue;
        if (!strncasecmp(p, "FILE", 4) && isspace((unsigned char)p[4])) {
            p += 5;
            while (isspace((unsigned char)*p))
                p++;
            char name[256], type[16];
            char *end;
            if (*p == '"') {
                end = strchr(++p, '"');
            } else {
                end = p;
                while (*end && !isspace((unsigned char)*end))
                    end++;
            }
            if (!end) {
                snprintf(err, errlen, "bad FILE line");
                ok = false;
                break;
            }
            size_t n = end - p;
            if (n >= sizeof(name))
                n = sizeof(name) - 1;
            memcpy(name, p, n);
            name[n] = 0;
            p = end + (*end == '"');
            if (sscanf(p, "%15s", type) != 1 || strcasecmp(type, "BINARY")) {
                snprintf(err, errlen, "file type %s is not supported (only BINARY): %s",
                         sscanf(p, "%15s", type) == 1 ? type : "?", name);
                ok = false;
                break;
            }
            if (img->nfiles >= MAX_FILES) {
                snprintf(err, errlen, "too many FILE lines");
                ok = false;
                break;
            }
            char fpath[520];
            snprintf(fpath, sizeof(fpath), "%s/%s", dir, name);
            if (file_size(fpath) < 0) {
                snprintf(err, errlen, "missing file: %s", name);
                ok = false;
                break;
            }
            first_track[img->nfiles] = img->ntracks;
            img->files[img->nfiles++] = dup_path(fpath);
        } else if (!strncasecmp(p, "TRACK", 5)) {
            int num;
            char mode[16];
            if (sscanf(p + 5, "%d %15s", &num, mode) != 2) {
                snprintf(err, errlen, "bad TRACK line");
                ok = false;
                break;
            }
            if (!img->nfiles) {
                snprintf(err, errlen, "TRACK before FILE");
                ok = false;
                break;
            }
            int m;
            for (m = 0; m < (int)NMODES; m++)
                if (!strcasecmp(mode, modes[m].name))
                    break;
            if (m == (int)NMODES) {
                snprintf(err, errlen, "unsupported track mode: %s", mode);
                ok = false;
                break;
            }
            if (img->ntracks >= MAX_TRACKS) {
                snprintf(err, errlen, "too many tracks");
                ok = false;
                break;
            }
            track_t *t = &img->tracks[img->ntracks];
            memset(t, 0, sizeof(*t));
            t->number = num;
            t->mode = m;
            t->sector_size = modes[m].size;
            t->audio = modes[m].skip < 0;
            t->skip = t->audio ? 0 : modes[m].skip;
            t->file = img->nfiles - 1;
            index1[img->ntracks] = -1;
            img->ntracks++;
        } else if (!strncasecmp(p, "INDEX", 5)) {
            int idx;
            char msf[16];
            if (sscanf(p + 5, "%d %15s", &idx, msf) == 2 && idx == 1 && img->ntracks)
                index1[img->ntracks - 1] = msf_to_frames(msf);
        } else if (!strncasecmp(p, "PREGAP", 6)) {
            warn(img, "PREGAP is not supported, ignored");
        } else if (!strncasecmp(p, "POSTGAP", 7)) {
            warn(img, "POSTGAP is not supported, ignored");
        }
    }
    fclose(f);
    if (!ok)
        return false;
    if (!img->ntracks) {
        snprintf(err, errlen, "no tracks in the .cue file");
        return false;
    }

    /* Absolute positions: the files follow each other on the disc. */
    uint32_t lba = 0;
    for (int fi = 0; fi < img->nfiles; fi++) {
        int t0 = first_track[fi];
        int t1 = fi + 1 < img->nfiles ? first_track[fi + 1] : img->ntracks;
        if (t0 >= t1)
            continue;
        uint32_t file_sectors = file_size(img->files[fi]) / img->tracks[t0].sector_size;
        for (int ti = t0; ti < t1; ti++) {
            track_t *t = &img->tracks[ti];
            if (index1[ti] < 0) {
                snprintf(err, errlen, "track %d has no INDEX 01", t->number);
                return false;
            }
            uint32_t end = ti + 1 < t1 ? (uint32_t)index1[ti + 1] : file_sectors;
            t->start = lba + index1[ti];
            t->length = end > (uint32_t)index1[ti] ? end - index1[ti] : 0;
            t->file_offset = (uint32_t)index1[ti] * t->sector_size;
        }
        lba += file_sectors;
    }
    return true;
}

/* ---- MDF/MDS (Alcohol 120%) ----
 * The .mds is a small binary file: a header, one block per session, and one
 * block per track with the mode, the sector size, the start LBA and the byte
 * offset in the .mdf. The .mdf holds the raw sectors, 2352 bytes each, or
 * 2448 with the subchannel. The .mdf has the same name as the .mds.
 * Layout as documented by the libmirage project. All values little endian. */
#define MDS_HEADER_LEN      0x58
#define MDS_SESSION_LEN     0x18
#define MDS_TRACK_LEN       0x50
#define MDS_MODE_AUDIO      0xA9
#define MDS_MODE_MODE1      0xAA
#define MDS_MODE_MODE2      0xAB
#define MDS_MODE_MODE2_F1   0xEC
#define MDS_MODE_MODE2_F2   0xED

static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool load_mds(image_t *img, const char *full, char *err, size_t errlen)
{
    FILE *f = fopen(full, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open the .mds file");
        return false;
    }
    uint8_t hdr[MDS_HEADER_LEN];
    bool ok = fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && !memcmp(hdr, "MEDIA DESCRIPTOR", 16);
    if (!ok) {
        fclose(f);
        snprintf(err, errlen, "not an .mds file");
        return false;
    }
    if (le16(hdr + 0x12) >= 0x10) {
        fclose(f);
        snprintf(err, errlen, "the .mds is a DVD image");
        return false;
    }
    /* the .mdf next to the .mds */
    char *mdf = dup_path(full);
    if (!mdf) {
        fclose(f);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    strcpy(mdf + strlen(mdf) - 4, ".mdf");
    img->files[0] = mdf;
    img->nfiles = 1;
    long mdf_size = file_size(mdf);
    if (mdf_size <= 0) {
        fclose(f);
        snprintf(err, errlen, "the .mdf file is missing");
        return false;
    }

    int nsessions = le16(hdr + 0x14);
    uint32_t sess_off = le32(hdr + 0x50);
    for (int si = 0; si < nsessions && ok; si++) {
        uint8_t sess[MDS_SESSION_LEN];
        if (fseek(f, sess_off + si * MDS_SESSION_LEN, SEEK_SET) || fread(sess, 1, sizeof(sess), f) != sizeof(sess)) {
            ok = false;
            break;
        }
        int nblocks = sess[0x0A];
        uint32_t trk_off = le32(sess + 0x14);
        for (int bi = 0; bi < nblocks && ok; bi++) {
            uint8_t b[MDS_TRACK_LEN];
            if (fseek(f, trk_off + bi * MDS_TRACK_LEN, SEEK_SET) || fread(b, 1, sizeof(b), f) != sizeof(b)) {
                ok = false;
                break;
            }
            int mode = b[0], point = b[4];
            if (point < 1 || point > 99 || mode == 0)
                continue;                   /* lead-in entries, not tracks */
            int m;
            uint16_t size = le16(b + 0x10);
            if (mode == MDS_MODE_AUDIO)
                m = 4;
            else if (mode == MDS_MODE_MODE1)
                m = size == 2048 ? 0 : 1;
            else if (mode == MDS_MODE_MODE2 || mode == MDS_MODE_MODE2_F1 || mode == MDS_MODE_MODE2_F2)
                m = size == 2336 ? 2 : 3;
            else {
                snprintf(err, errlen, "unsupported track mode 0x%02X in the .mds", mode);
                ok = false;
                break;
            }
            if (img->ntracks >= MAX_TRACKS) {
                snprintf(err, errlen, "too many tracks");
                ok = false;
                break;
            }
            if (size != 2048 && size != 2336 && size != 2352 && size != 2448) {
                snprintf(err, errlen, "unsupported sector size %u in the .mds", size);
                ok = false;
                break;
            }
            track_t *t = &img->tracks[img->ntracks++];
            memset(t, 0, sizeof(*t));
            t->number = point;
            t->mode = m;
            t->sector_size = size;          /* 2448 keeps the mode name, the stride differs */
            t->audio = modes[m].skip < 0;
            t->skip = t->audio ? 0 : modes[m].skip;
            t->file = 0;
            t->start = le32(b + 0x24);
            t->file_offset = le32(b + 0x28);  /* low 32 bits of the 64-bit offset: CD images fit */
            uint32_t extra = le32(b + 0x0C);
            if (extra) {                    /* the extra block holds pregap and length */
                uint8_t x[8];
                if (!fseek(f, extra, SEEK_SET) && fread(x, 1, 8, f) == 8)
                    t->length = le32(x + 4);
            }
        }
    }
    fclose(f);
    if (!ok) {
        if (!err[0])
            snprintf(err, errlen, "the .mds file is damaged");
        return false;
    }
    if (!img->ntracks) {
        snprintf(err, errlen, "no tracks in the .mds file");
        return false;
    }
    /* A track without a length runs to the next track, or to the file end. */
    for (int i = 0; i < img->ntracks; i++) {
        track_t *t = &img->tracks[i];
        if (t->length)
            continue;
        if (i + 1 < img->ntracks)
            t->length = img->tracks[i + 1].start > t->start ? img->tracks[i + 1].start - t->start : 0;
        else
            t->length = ((uint32_t)mdf_size - t->file_offset) / t->sector_size;
    }
    if (img->tracks[0].sector_size == 2448)
        warn(img, "subchannel data in the .mdf, reads are slower");
    return true;
}

image_t *image_open(const char *rel_path, char *err, size_t errlen)
{
    if (strstr(rel_path, "..")) {
        snprintf(err, errlen, "not in the library: %s", rel_path);
        return NULL;
    }
    image_t *img = heap_caps_calloc(1, sizeof(image_t), MALLOC_CAP_SPIRAM);
    if (!img)
        img = calloc(1, sizeof(image_t));
    if (!img) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    strlcpy(img->path, rel_path, sizeof(img->path));
    char full[300];
    snprintf(full, sizeof(full), "%s/%s", SD_MOUNT, rel_path);
    size_t n = strlen(full);
    bool ok;
    sd_lock();
    err[0] = 0;
    if (n > 4 && !strcasecmp(full + n - 4, ".cue"))
        ok = load_cue(img, full, err, errlen);
    else if (n > 4 && !strcasecmp(full + n - 4, ".mds"))
        ok = load_mds(img, full, err, errlen);
    else
        ok = load_iso(img, full, err, errlen);
    sd_unlock();
    if (!ok) {
        image_close(img);
        return NULL;
    }
    const track_t *last = &img->tracks[img->ntracks - 1];
    img->sectors = last->start + last->length;
    return img;
}

void image_close(image_t *img)
{
    if (!img)
        return;
    image_files_flush();
    for (int i = 0; i < img->nfiles; i++)
        free(img->files[i]);
    free(img);
}

const track_t *image_track_at(const image_t *img, uint32_t lba)
{
    for (int i = 0; i < img->ntracks; i++) {
        const track_t *t = &img->tracks[i];
        if (lba >= t->start && lba < t->start + t->length)
            return t;
    }
    return NULL;
}

/* Cooked 2048-byte sectors. Reads runs within one track in one call. */
int image_read(image_t *img, uint32_t lba, int count, uint8_t *out)
{
    if (!raw)
        return -1;
    int rc = 0;
    sd_lock();
    while (count > 0) {
        const track_t *t = image_track_at(img, lba);
        if (!t || t->audio) {
            rc = -1;
            break;
        }
        uint32_t left = t->start + t->length - lba;
        int n = count < (int)left ? count : (int)left;
        if (n > 8)
            n = 8;
        const char *path = img->files[t->file];
        uint32_t off = t->file_offset + (lba - t->start) * t->sector_size;
        if (t->sector_size == 2048) {
            if (read_at(path, off, out, n * 2048) != n * 2048) {
                rc = -1;
                break;
            }
        } else {
            size_t want = (size_t)n * t->sector_size;
            if (read_at(path, off, raw, want) != (int)want) {
                rc = -1;
                break;
            }
            for (int i = 0; i < n; i++)
                memcpy(out + i * 2048, raw + i * t->sector_size + t->skip, 2048);
        }
        out += n * 2048;
        lba += n;
        count -= n;
    }
    sd_unlock();
    return rc;
}

/* Raw 2352-byte audio sectors. Data sectors and gaps are silence. */
int image_read_audio(image_t *img, uint32_t lba, int count, uint8_t *out)
{
    sd_lock();
    while (count > 0) {
        const track_t *t = image_track_at(img, lba);
        if (!t || !t->audio) {
            memset(out, 0, 2352);
            out += 2352;
            lba++;
            count--;
            continue;
        }
        uint32_t left = t->start + t->length - lba;
        int n = count < (int)left ? count : (int)left;
        if (t->sector_size != 2352)
            n = 1;                          /* with subchannel bytes: one sector at a time */
        uint32_t off = t->file_offset + (lba - t->start) * t->sector_size;
        int got = read_at(img->files[t->file], off, out, (size_t)n * 2352);
        if (got < n * 2352)
            memset(out + (got > 0 ? got : 0), 0, n * 2352 - (got > 0 ? got : 0));
        out += n * 2352;
        lba += n;
        count -= n;
    }
    sd_unlock();
    return 0;
}

static void redbook(uint32_t lba, uint8_t *out)
{
    lba += 150;
    out[0] = lba % 75;
    out[1] = (lba / 75) % 60;
    out[2] = lba / 4500;
    out[3] = 0;
}

size_t image_offer_body(const image_t *img, uint8_t disc_id, uint8_t *out)
{
    uint8_t *p = out;
    memset(p, 0, 16);
    const char *base = strrchr(img->path, '/');
    base = base ? base + 1 : img->path;
    size_t n = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot)
        n = dot - base;
    for (size_t i = 0; i < n && i < 16; i++)
        p[i] = (base[i] & 0x80) ? '?' : base[i];
    p += 16;
    memcpy(p, &img->sectors, 4);
    p += 4;
    *p++ = img->tracks[0].number;
    *p++ = img->tracks[img->ntracks - 1].number;
    *p++ = img->ntracks;
    *p++ = disc_id;
    redbook(img->sectors, p);
    p += 4;
    for (int i = 0; i < img->ntracks; i++) {
        *p++ = img->tracks[i].number;
        *p++ = img->tracks[i].audio ? 0x01 : 0x41;
        redbook(img->tracks[i].start, p);
        p += 4;
    }
    *p++ = PROTOCOL_VERSION;
    return p - out;
}

size_t empty_offer_body(uint8_t disc_id, uint8_t *out)
{
    memset(out, 0, 28);
    memcpy(out, "NO DISC", 7);
    out[23] = disc_id;
    out[28] = PROTOCOL_VERSION;
    return 29;
}
