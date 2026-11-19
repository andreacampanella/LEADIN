/* stats.c - counters for the web UI: read speed per second for the last minute,
 * totals, the clients seen, and the event log.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "leadin.h"

#define MAX_CLIENTS     8

typedef struct {
    uint32_t ip;
    int64_t  last;
    uint32_t reads, kbytes, retries;
    uint16_t last_seq;
} client_t;

static SemaphoreHandle_t stats_lock;

/* "a.b.c.d" from an address in network byte order. out needs 16 bytes. */
void ip_str(uint32_t ip, char *out)
{
    const uint8_t *b = (const uint8_t *)&ip;
    sprintf(out, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}
static client_t clients[MAX_CLIENTS];
static uint32_t speed[60];
static int64_t speed_second;
static uint32_t speed_bytes;
static uint32_t total_reads, total_kbytes, total_retries, total_errors;

static void roll(void)
{
    int64_t now = esp_timer_get_time() / 1000000;
    if (now - speed_second > 60)
        speed_second = now - 60;
    while (speed_second < now) {
        memmove(speed, speed + 1, sizeof(speed) - sizeof(speed[0]));
        speed[59] = speed_bytes;
        speed_bytes = 0;
        speed_second++;
    }
}

static client_t *client_for(uint32_t ip)
{
    int oldest = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].ip == ip)
            return &clients[i];
        if (clients[i].last < clients[oldest].last)
            oldest = i;
    }
    memset(&clients[oldest], 0, sizeof(client_t));
    clients[oldest].ip = ip;
    clients[oldest].last_seq = 0xFFFF;
    return &clients[oldest];
}

void stats_start(void)
{
    stats_lock = xSemaphoreCreateMutex();
}

void stats_seen(uint32_t ip)
{
    xSemaphoreTake(stats_lock, portMAX_DELAY);
    client_for(ip)->last = esp_timer_get_time();
    xSemaphoreGive(stats_lock);
}

void stats_read(uint32_t ip, uint16_t seq, uint32_t bytes, bool error)
{
    xSemaphoreTake(stats_lock, portMAX_DELAY);
    roll();
    client_t *c = client_for(ip);
    c->last = esp_timer_get_time();
    if (c->last_seq == seq) {
        c->retries++;
        total_retries++;
    }
    c->last_seq = seq;
    if (error) {
        total_errors++;
    } else {
        c->reads++;
        c->kbytes += bytes / 1024;
        total_reads++;
        total_kbytes += bytes / 1024;
        speed_bytes += bytes;
    }
    xSemaphoreGive(stats_lock);
}

void stats_event(const char *fmt, ...)
{
    char buf[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    nlog("%s", buf);
}


void stats_json(cJSON *obj)
{
    xSemaphoreTake(stats_lock, portMAX_DELAY);
    roll();
    cJSON *sp = cJSON_AddArrayToObject(obj, "speed");
    for (int i = 0; i < 60; i++)
        cJSON_AddItemToArray(sp, cJSON_CreateNumber(speed[i]));
    cJSON_AddNumberToObject(obj, "reads", total_reads);
    cJSON_AddNumberToObject(obj, "kbytes", total_kbytes);
    cJSON_AddNumberToObject(obj, "retries", total_retries);
    cJSON_AddNumberToObject(obj, "errors", total_errors);
    cJSON *cl = cJSON_AddArrayToObject(obj, "clients");
    /* most recent first */
    bool done[MAX_CLIENTS] = {0};
    int64_t now = esp_timer_get_time();
    for (;;) {
        int best = -1;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].ip && !done[i] && (best < 0 || clients[i].last > clients[best].last))
                best = i;
        if (best < 0)
            break;
        done[best] = true;
        char ip[16];
        ip_str(clients[best].ip, ip);
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "ip", ip);
        cJSON_AddNumberToObject(c, "seconds_ago", (now - clients[best].last) / 100000 / 10.0);
        cJSON_AddNumberToObject(c, "reads", clients[best].reads);
        cJSON_AddNumberToObject(c, "kbytes", clients[best].kbytes);
        cJSON_AddNumberToObject(c, "retries", clients[best].retries);
        cJSON_AddItemToArray(cl, c);
    }
    xSemaphoreGive(stats_lock);
}
