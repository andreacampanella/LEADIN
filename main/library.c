/* library.c - the images on the SD card: .iso, .cue and .mds files in the root and
 * all subfolders (folders that start with a dot are skipped), sorted by
 * path, case ignored. LIST indexes (LEADOUT /DISC) follow this order.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "leadin.h"

#define MAX_ENTRIES 512
#define MAX_DEPTH   4
#define RESCAN_US   (5 * 1000000LL)     /* a rescan at most this often ... */
#define QUIET_US    (3 * 1000000LL)     /* ... and only after this long without a DOS read */

static lib_entry_t entries[MAX_ENTRIES];
static int count;
static int64_t scanned_at = -RESCAN_US;

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

/* Size of an .mds file and its .mdf, in KB. */
static uint32_t mds_kbytes(const char *full)
{
    struct stat st;
    uint64_t total = stat(full, &st) == 0 ? st.st_size : 0;
    char mdf[300];
    strlcpy(mdf, full, sizeof(mdf));
    strcpy(mdf + strlen(mdf) - 4, ".mdf");
    if (stat(mdf, &st) == 0)
        total += st.st_size;
    return total / 1024;
}

/* Size of a .cue file and the files its FILE lines name, in KB. */
static uint32_t cue_kbytes(const char *full, const char *dir)
{
    struct stat st;
    uint64_t total = stat(full, &st) == 0 ? st.st_size : 0;
    FILE *f = fopen(full, "r");
    if (!f)
        return total / 1024;
    char line[300], path[300];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncasecmp(p, "FILE", 4) != 0)
            continue;
        p += 4;
        while (*p == ' ' || *p == '\t')
            p++;
        char *start = p, *end;
        if (*p == '"') {
            start = ++p;
            end = strchr(p, '"');
        } else {
            end = strpbrk(p, " \t");
        }
        if (!end)
            continue;
        *end = 0;
        snprintf(path, sizeof(path), "%s/%s", dir, start);
        if (stat(path, &st) == 0)
            total += st.st_size;
    }
    fclose(f);
    return total / 1024;
}

static void add(const char *rel, uint32_t kb)
{
    if (count >= MAX_ENTRIES)
        return;
    size_t n = strlen(rel) + 1;
    char *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!p)
        p = malloc(n);
    if (!p)
        return;
    memcpy(p, rel, n);
    entries[count].path = p;
    entries[count].kbytes = kb;
    count++;
}

static void scan_dir(const char *full, const char *rel, int depth)
{
    DIR *d = opendir(full);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char fpath[300], rpath[300];
        snprintf(fpath, sizeof(fpath), "%s/%s", full, e->d_name);
        snprintf(rpath, sizeof(rpath), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        if (e->d_type == DT_DIR) {
            if (depth < MAX_DEPTH)
                scan_dir(fpath, rpath, depth + 1);
        } else if (has_ext(e->d_name, ".iso")) {
            struct stat st;
            add(rpath, stat(fpath, &st) == 0 ? st.st_size / 1024 : 0);
        } else if (has_ext(e->d_name, ".cue")) {
            add(rpath, cue_kbytes(fpath, full));
        } else if (has_ext(e->d_name, ".mds")) {
            add(rpath, mds_kbytes(fpath));
        }
    }
    closedir(d);
}

static int cmp(const void *a, const void *b)
{
    return strcasecmp(((const lib_entry_t *)a)->path, ((const lib_entry_t *)b)->path);
}

/* The scan walks every folder and opens every .cue, a third of a second
 * with a few dozen discs, under the card lock. A DOS read that arrives
 * meanwhile waits. So the timed rescan (from the web UI's poll) runs only
 * while DOS is quiet. A mount and an upload force it. */
int library_scan(bool force)
{
    int64_t now = esp_timer_get_time();
    sd_lock();
    if (!force && (now - scanned_at < RESCAN_US || now - leadin_last_read_us() < QUIET_US)) {
        int n = count;
        sd_unlock();
        return n;
    }
    for (int i = 0; i < count; i++)
        free(entries[i].path);
    count = 0;
    if (sd_present())
        scan_dir(SD_MOUNT, "", 0);
    qsort(entries, count, sizeof(entries[0]), cmp);
    scanned_at = now;
    int n = count;
    sd_unlock();
    return n;
}

bool library_get(int index, char *path, size_t len, uint32_t *kbytes)
{
    bool ok = false;
    sd_lock();
    if (index >= 0 && index < count) {
        strlcpy(path, entries[index].path, len);
        if (kbytes)
            *kbytes = entries[index].kbytes;
        ok = true;
    }
    sd_unlock();
    return ok;
}

int library_find(const char *path)
{
    int found = -1;
    sd_lock();
    for (int i = 0; i < count; i++)
        if (!strcmp(entries[i].path, path)) {
            found = i;
            break;
        }
    sd_unlock();
    return found;
}
