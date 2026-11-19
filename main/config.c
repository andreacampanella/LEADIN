/* config.c - LEADIN.CFG on the SD card, and the settings in NVS
 *
 * LEADIN.CFG holds one KEY=value per line. Keys (case ignored):
 *   SSID, PASSWORD, HOSTNAME, LINE_LEVEL,
 *   I2S_WS, I2S_DOUT (pin overrides for some Audio Kit batches)
 * Lines that start with # or ; are comments.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "leadin.h"
#include "nvs.h"
#include "nvs_flash.h"

leadin_cfg_t g_cfg;
settings_t  g_set;

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

void config_load_file(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    strlcpy(g_cfg.hostname, DEFAULT_HOSTNAME, sizeof(g_cfg.hostname));
    g_cfg.line_level = DEFAULT_LINE_LEVEL;

    sd_lock();
    FILE *f = fopen(CFG_FILE, "r");
    if (!f) {
        sd_unlock();
        nlog("LEADIN.CFG not found on the SD card");
        return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        if (!strcasecmp(key, "SSID")) {
            strlcpy(g_cfg.ssid, val, sizeof(g_cfg.ssid));
            g_cfg.have_wifi = g_cfg.ssid[0] != 0;
        } else if (!strcasecmp(key, "PASSWORD")) {
            strlcpy(g_cfg.password, val, sizeof(g_cfg.password));
        } else if (!strcasecmp(key, "HOSTNAME")) {
            if (*val)
                strlcpy(g_cfg.hostname, val, sizeof(g_cfg.hostname));
        } else if (!strcasecmp(key, "I2S_WS")) {
            g_cfg.i2s_ws = atoi(val);
        } else if (!strcasecmp(key, "I2S_DOUT")) {
            g_cfg.i2s_dout = atoi(val);
        } else if (!strcasecmp(key, "LINE_LEVEL")) {
            int v = atoi(val);
            if (v >= 0 && v <= 33)
                g_cfg.line_level = v;
        } else {
            nlog("LEADIN.CFG: unknown key %s", key);
        }
    }
    fclose(f);
    sd_unlock();
    nlog("LEADIN.CFG read: network '%s', host name %s", g_cfg.ssid, g_cfg.hostname);
}

bool config_write_wifi(const char *ssid, const char *password)
{
    sd_lock();
    FILE *f = fopen(CFG_FILE, "w");
    if (!f) {
        sd_unlock();
        return false;
    }
    fprintf(f, "# LEADIN settings, written by the setup page\r\n");
    fprintf(f, "SSID=%s\r\nPASSWORD=%s\r\nHOSTNAME=%s\r\nLINE_LEVEL=%d\r\n",
            ssid, password, g_cfg.hostname[0] ? g_cfg.hostname : DEFAULT_HOSTNAME,
            g_cfg.line_level ? g_cfg.line_level : DEFAULT_LINE_LEVEL);
    bool ok = fclose(f) == 0;
    sd_unlock();
    return ok;
}

void settings_load(void)
{
    g_set.volume = 255;
    g_set.mute = false;
    g_set.override = false;
    g_set.client_timeout = DEFAULT_CLIENT_TIMEOUT;
    g_set.pc_volume = DEFAULT_PC_VOLUME;
    g_set.pc_mute = false;
    g_set.pc_input = 0;
    g_set.disc[0] = 0;

    nvs_handle_t h;
    if (nvs_open("leadin", NVS_READONLY, &h) != ESP_OK)
        return;
    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, "volume", &u8) == ESP_OK) g_set.volume = u8;
    if (nvs_get_u8(h, "mute", &u8) == ESP_OK) g_set.mute = u8;
    if (nvs_get_u8(h, "override", &u8) == ESP_OK) g_set.override = u8;
    if (nvs_get_u16(h, "timeout", &u16) == ESP_OK) g_set.client_timeout = u16;
    if (nvs_get_u8(h, "pcvol", &u8) == ESP_OK && u8 <= 100) g_set.pc_volume = u8;
    if (nvs_get_u8(h, "pcmute", &u8) == ESP_OK) g_set.pc_mute = u8;
    if (nvs_get_u8(h, "pcin", &u8) == ESP_OK && u8 <= 2) g_set.pc_input = u8;
    if (nvs_get_u8(h, "light", &u8) == ESP_OK) g_set.light_theme = u8;
    /* ota_enabled is not stored: the switch is off after every start */
    size_t len = sizeof(g_set.disc);
    if (nvs_get_str(h, "disc", g_set.disc, &len) != ESP_OK)
        g_set.disc[0] = 0;
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("leadin", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, "volume", g_set.volume);
    nvs_set_u8(h, "mute", g_set.mute);
    nvs_set_u8(h, "override", g_set.override);
    nvs_set_u16(h, "timeout", g_set.client_timeout);
    nvs_set_u8(h, "pcvol", g_set.pc_volume);
    nvs_set_u8(h, "pcmute", g_set.pc_mute);
    nvs_set_u8(h, "pcin", g_set.pc_input);
    nvs_set_u8(h, "light", g_set.light_theme);
    nvs_set_str(h, "disc", g_set.disc);
    nvs_commit(h);
    nvs_close(h);
}
