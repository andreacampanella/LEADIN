/* webui.c - the web UI and its JSON API
 *
 * Serves webui.html (embedded at build time) and the JSON API:
 * /api/state, /api/select, /api/audio, /api/volume, /api/export, /api/upload,
 * /api/system (the OTA switch) and /api/firmware (the OTA upload).
 * In setup mode, it serves only the Wi-Fi setup page.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leadin.h"

extern const char webui_html_start[] asm("_binary_webui_html_start");
extern const char webui_html_end[] asm("_binary_webui_html_end");

static httpd_handle_t server;

/* ---------------------------------------------------------------- helpers */
static esp_err_t send_json(httpd_req_t *req, cJSON *obj, const char *status)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (status)
        httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, s ? s : "{}", HTTPD_RESP_USE_STRLEN);
    free(s);
    return r;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, o, status);
}

static cJSON *read_body(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 4096)
        return cJSON_CreateObject();
    char *buf = malloc(len + 1);
    if (!buf)
        return NULL;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[len] = 0;
    cJSON *o = cJSON_Parse(buf);
    free(buf);
    return o ? o : cJSON_CreateObject();
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else if (*s == '+') {
            *o++ = ' ';
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static bool query(httpd_req_t *req, const char *key, char *val, size_t len)
{
    char q[400];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK)
        return false;
    if (httpd_query_key_value(q, key, val, len) != ESP_OK)
        return false;
    url_decode(val);
    return true;
}

static double round1(double v)
{
    return (int64_t)(v * 10 + (v >= 0 ? 0.5 : -0.5)) / 10.0;
}

/* ------------------------------------------------------------------ state */
static cJSON *web_state(void)
{
    cJSON *root = cJSON_CreateObject();
    int state;
    uint32_t pos, start, end;
    audio_status(&state, &pos, &start, &end);
    uint32_t owner = audio_owner();
    uint8_t gl, gr;
    audio_get_game_volume(&gl, &gr);
    static const char *names[] = {"stopped", "playing", "paused"};

    cJSON *a = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddStringToObject(a, "state", names[state]);
    cJSON_AddNullToObject(a, "track");
    cJSON_AddNumberToObject(a, "pos", 0);
    cJSON_AddNumberToObject(a, "length", 0);
    cJSON_AddNumberToObject(a, "volume", (g_set.volume * 100 + 127) / 255);
    cJSON_AddBoolToObject(a, "mute", g_set.mute);
    cJSON_AddBoolToObject(a, "override", g_set.override);
    cJSON_AddNumberToObject(a, "game_volume", (gl * 100 + 127) / 255);
    const char *out = audio_output_name();
    if (!out)
        cJSON_AddNullToObject(a, "output");
    else
        cJSON_AddStringToObject(a, "output", out);
    if (owner == OWNER_NONE)
        cJSON_AddNullToObject(a, "owner");
    else
        cJSON_AddStringToObject(a, "owner", owner == OWNER_WEB ? "web" : "dos");
    cJSON_AddNumberToObject(a, "client_timeout", g_set.client_timeout);
    if (mixer_available()) {
        cJSON *m = cJSON_AddObjectToObject(a, "mixer");
        cJSON_AddNumberToObject(m, "volume", g_set.pc_volume);
        cJSON_AddBoolToObject(m, "mute", g_set.pc_mute);
        if (mixer_input())
            cJSON_AddNumberToObject(m, "input", mixer_input());
    } else {
        cJSON_AddNullToObject(a, "mixer");
    }

    sd_lock();
    image_t *img = disc_current();
    char cur_path[256] = "";
    if (img) {
        strlcpy(cur_path, img->path, sizeof(cur_path));
        const track_t *t = state ? image_track_at(img, pos) : NULL;
        if (t && t->audio) {
            cJSON_ReplaceItemInObject(a, "track", cJSON_CreateNumber(t->number));
            cJSON_ReplaceItemInObject(a, "pos", cJSON_CreateNumber(round1((pos - t->start) / 75.0)));
            cJSON_ReplaceItemInObject(a, "length", cJSON_CreateNumber(round1(t->length / 75.0)));
        }
        cJSON *d = cJSON_AddObjectToObject(root, "disc");
        cJSON_AddStringToObject(d, "name", img->path);
        cJSON_AddNumberToObject(d, "sectors", img->sectors);
        cJSON_AddNumberToObject(d, "mb", (double)img->sectors * 2048 / 1048576);
        cJSON *w = cJSON_AddArrayToObject(d, "warnings");
        if (img->warnings[0])
            cJSON_AddItemToArray(w, cJSON_CreateString(img->warnings));
        cJSON *tr = cJSON_AddArrayToObject(d, "tracks");
        for (int i = 0; i < img->ntracks; i++) {
            const track_t *k = &img->tracks[i];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "number", k->number);
            cJSON_AddStringToObject(o, "mode", image_mode_name(k));
            cJSON_AddBoolToObject(o, "audio", k->audio);
            cJSON_AddNumberToObject(o, "start", k->start);
            cJSON_AddNumberToObject(o, "seconds", round1(k->length / 75.0));
            cJSON_AddItemToArray(tr, o);
        }
    } else {
        cJSON_AddNullToObject(root, "disc");
    }
    sd_unlock();

    cJSON *lib = cJSON_AddArrayToObject(root, "library");
    int n = library_scan(false);
    char path[256];
    uint32_t kb;
    for (int i = 0; i < n; i++) {
        if (!library_get(i, path, sizeof(path), &kb))
            break;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", path);
        cJSON_AddNumberToObject(o, "mb", round1(kb / 1024.0));
        cJSON_AddBoolToObject(o, "current", !strcmp(path, cur_path));
        cJSON_AddItemToArray(lib, o);
    }

    stats_json(cJSON_AddObjectToObject(root, "activity"));
    cJSON_AddNumberToObject(root, "udp_port", LEADIN_UDP_PORT);

    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(dev, "firmware", LEADIN_VERSION);
    cJSON_AddStringToObject(dev, "board", g_board.name);
    cJSON_AddStringToObject(dev, "ip", net_ip());
    cJSON_AddNumberToObject(dev, "rssi", net_rssi());
    cJSON_AddNumberToObject(dev, "free_heap", esp_get_free_heap_size());
    cJSON_AddBoolToObject(dev, "sd_card", sd_present());
    cJSON_AddBoolToObject(dev, "ota_enabled", g_set.ota_enabled);
    cJSON_AddBoolToObject(dev, "light_theme", g_set.light_theme);
    cJSON_AddStringToObject(dev, "ota_slot", ota_running());
    cJSON_AddNumberToObject(dev, "uptime", esp_timer_get_time() / 1000000);
    return root;
}

/* --------------------------------------------------------------- handlers */
static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, webui_html_start, webui_html_end - webui_html_start - 1);
}

static esp_err_t h_state(httpd_req_t *req)
{
    return send_json(req, web_state(), NULL);
}

static esp_err_t h_select(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (!b)
        return send_error(req, "400 Bad Request", "bad request body");
    cJSON *name = cJSON_GetObjectItem(b, "name");
    char err[120] = "";
    bool ok = true;
    if (cJSON_IsString(name) && name->valuestring[0])
        ok = disc_select(name->valuestring, err, sizeof(err));
    else
        disc_eject();
    cJSON_Delete(b);
    if (!ok)
        return send_error(req, "400 Bad Request", err);
    return send_json(req, web_state(), NULL);
}

static esp_err_t h_audio(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (!b)
        return send_error(req, "400 Bad Request", "bad request body");
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(b, "action"));
    const char *err = NULL;
    if (!action) {
        err = "unknown action";
    } else if (!strcmp(action, "play")) {
        int num = cJSON_GetNumberValue(cJSON_GetObjectItem(b, "track"));
        uint32_t start = 0, len = 0;
        sd_lock();
        image_t *img = disc_current();
        for (int i = 0; img && i < img->ntracks; i++)
            if (img->tracks[i].number == num && img->tracks[i].audio) {
                start = img->tracks[i].start;
                len = img->tracks[i].length;
            }
        sd_unlock();
        if (!len)
            err = "no such audio track";
        else if (!audio_play(start, len, OWNER_WEB))
            err = "audio output is off";
        else
            stats_event("PLAY from web UI: track %d", num);
    } else if (!strcmp(action, "pause")) {
        audio_pause();
    } else if (!strcmp(action, "resume")) {
        audio_resume();
    } else if (!strcmp(action, "stop")) {
        audio_reset();
    } else {
        err = "unknown action";
    }
    cJSON_Delete(b);
    if (err)
        return send_error(req, "400 Bad Request", err);
    return send_json(req, web_state(), NULL);
}

static esp_err_t h_volume(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (!b)
        return send_error(req, "400 Bad Request", "bad request body");
    cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(b, "volume"))) {
        double p = v->valuedouble < 0 ? 0 : v->valuedouble > 100 ? 100 : v->valuedouble;
        g_set.volume = (uint8_t)(p * 255 / 100 + 0.5);
    }
    if (cJSON_IsBool(v = cJSON_GetObjectItem(b, "mute")))
        g_set.mute = cJSON_IsTrue(v);
    if (cJSON_IsBool(v = cJSON_GetObjectItem(b, "override"))) {
        g_set.override = cJSON_IsTrue(v);
        nlog("Music override %s", g_set.override ? "on" : "off");
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(b, "client_timeout"))) {
        int t = v->valueint;
        g_set.client_timeout = t < 0 ? 0 : t > 3600 ? 3600 : t;
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(b, "pc_volume"))) {
        int p = v->valueint;
        g_set.pc_volume = p < 0 ? 0 : p > 100 ? 100 : p;
    }
    if (cJSON_IsBool(v = cJSON_GetObjectItem(b, "pc_mute")))
        g_set.pc_mute = cJSON_IsTrue(v);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(b, "pc_input")) && v->valueint >= 0 && v->valueint <= 2)
        g_set.pc_input = v->valueint;
    cJSON_Delete(b);
    audio_apply_volume();
    mixer_apply();
    settings_save();
    return send_json(req, web_state(), NULL);
}

static void wav_header(uint8_t *h, uint32_t n)
{
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + n;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;
    memcpy(h + 16, &v, 4);
    uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
    uint32_t rate = 44100, bps = 176400;
    memcpy(h + 20, &fmt, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &bps, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &n, 4);
}

static esp_err_t h_export(httpd_req_t *req)
{
    char v[8];
    if (!query(req, "track", v, sizeof(v)))
        return send_error(req, "404 Not Found", "no such audio track");
    int num = atoi(v);
    sd_lock();
    image_t *img = disc_current();
    uint8_t id = disc_id();
    const track_t *t = NULL;
    for (int i = 0; img && i < img->ntracks; i++)
        if (img->tracks[i].number == num && img->tracks[i].audio)
            t = &img->tracks[i];
    uint32_t start = t ? t->start : 0, len = t ? t->length : 0;
    char stem[64] = "disc";
    if (img) {
        const char *b = strrchr(img->path, '/');
        b = b ? b + 1 : img->path;
        size_t k = 0;
        for (; *b && *b != '.' && k < sizeof(stem) - 1; b++)
            stem[k++] = (isalnum((unsigned char)*b) || *b == '-' || *b == '_') ? *b : '_';
        stem[k] = 0;
    }
    sd_unlock();
    if (!len)
        return send_error(req, "404 Not Found", "no such audio track");

    char disp[120];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s-track%02d.wav\"", stem, num);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    uint8_t hdr[44];
    wav_header(hdr, len * 2352);
    if (httpd_resp_send_chunk(req, (char *)hdr, 44) != ESP_OK)
        return ESP_FAIL;

    const int per = 10;
    uint8_t *buf = heap_caps_malloc(per * 2352, MALLOC_CAP_SPIRAM);
    if (!buf)
        buf = malloc(per * 2352);
    if (!buf)
        return httpd_resp_send_chunk(req, NULL, 0);
    for (uint32_t lba = start; lba < start + len;) {
        int n = start + len - lba < (uint32_t)per ? start + len - lba : per;
        sd_lock();
        bool same = disc_current() == img && disc_id() == id;
        if (same)
            image_read_audio(img, lba, n, buf);
        sd_unlock();
        if (!same || httpd_resp_send_chunk(req, (char *)buf, n * 2352) != ESP_OK)
            break;
        lba += n;
    }
    free(buf);
    stats_event("Exported track %d as WAV", num);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool name_ok(const char *n)
{
    if (!*n || strchr(n, '/') || strchr(n, '\\') || !strcmp(n, ".") || strstr(n, ".."))
        return false;
    for (const char *p = n; *p; p++)
        if (!(isalnum((unsigned char)*p) || strchr("_ .()[]&'+,-", *p)))
            return false;
    size_t l = strlen(n);
    static const char *ext[] = {".iso", ".cue", ".bin", ".img", ".mds", ".mdf"};
    for (int i = 0; i < 4; i++)
        if (l > 4 && !strcasecmp(n + l - 4, ext[i]))
            return true;
    return false;
}

static esp_err_t h_upload(httpd_req_t *req)
{
    char name[200], ow[4] = "0";
    if (!query(req, "name", name, sizeof(name)) || !name_ok(name))
        return send_error(req, "400 Bad Request",
                          "file name or type not allowed (use .iso, .cue, .bin, .img, .mds, .mdf)");
    query(req, "overwrite", ow, sizeof(ow));
    char dest[300], tmp[310];
    snprintf(dest, sizeof(dest), "%s/%s", SD_MOUNT, name);
    snprintf(tmp, sizeof(tmp), "%s.part", dest);
    struct stat st;
    if (stat(dest, &st) == 0 && strcmp(ow, "1")) {
        char m[240];
        snprintf(m, sizeof(m), "%s already exists", name);
        return send_error(req, "409 Conflict", m);
    }
    const size_t bs = 16384;
    char *buf = heap_caps_malloc(bs, MALLOC_CAP_SPIRAM);
    if (!buf)
        buf = malloc(bs);
    FILE *f = buf ? fopen(tmp, "wb") : NULL;
    if (!f) {
        free(buf);
        return send_error(req, "500 Internal Server Error", "cannot write to the SD card");
    }
    size_t left = req->content_len;
    bool ok = true;
    while (left > 0) {
        int r = httpd_req_recv(req, buf, left < bs ? left : bs);
        if (r == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (r <= 0) {
            ok = false;
            break;
        }
        sd_lock();
        ok = fwrite(buf, 1, r, f) == (size_t)r;
        sd_unlock();
        if (!ok)
            break;
        left -= r;
    }
    sd_lock();
    ok &= fclose(f) == 0;
    if (ok) {
        unlink(dest);
        ok = rename(tmp, dest) == 0;
    }
    if (!ok)
        unlink(tmp);
    sd_unlock();
    free(buf);
    if (!ok)
        return send_error(req, "500 Internal Server Error", "upload interrupted or SD card full");
    library_scan(true);
    stats_event("Uploaded %s (%.1f MB)", name, req->content_len / 1048576.0);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "name", name);
    return send_json(req, o, NULL);
}

/* POST /api/system {"ota_enabled": true, "light_theme": false}: the switch for
 * /api/firmware (not kept), and the web UI colors (kept in NVS). */
static esp_err_t h_system(httpd_req_t *req)
{
    cJSON *b = read_body(req);
    if (!b)
        return send_error(req, "400 Bad Request", "bad request body");
    cJSON *v;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(b, "ota_enabled"))) {
        g_set.ota_enabled = cJSON_IsTrue(v);
        nlog("Firmware update %s", g_set.ota_enabled ? "enabled from the web UI" : "disabled");
    }
    if (cJSON_IsBool(v = cJSON_GetObjectItem(b, "light_theme"))) {
        g_set.light_theme = cJSON_IsTrue(v);
        settings_save();
    }
    cJSON_Delete(b);
    return send_json(req, web_state(), NULL);
}

static esp_err_t h_firmware(httpd_req_t *req)
{
    return ota_upload_handler(req);
}

/* ----------------------------------------------------------- setup portal */
static const char setup_page[] =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width'>"
    "<title>LEADIN setup</title><style>body{font:16px sans-serif;margin:2em;max-width:28em}"
    "input{width:100%;padding:.4em;margin:.3em 0 1em;font-size:16px}button{padding:.5em 1.2em;font-size:16px}"
    "</style></head><body><h2>LEADIN setup</h2><p>Enter your Wi-Fi network. The board saves it in "
    "LEADIN.CFG on the SD card and restarts.</p><form method='post' action='/setup'>"
    "<label>Network name (SSID)<input name='ssid' maxlength='32' required></label>"
    "<label>Password<input name='password' type='password' maxlength='64'></label>"
    "<button>Save and restart</button></form></body></html>";

static esp_err_t h_setup_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, setup_page, HTTPD_RESP_USE_STRLEN);
}

static void form_value(char *body, const char *key, char *out, size_t len)
{
    out[0] = 0;
    if (httpd_query_key_value(body, key, out, len) == ESP_OK)
        url_decode(out);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t h_setup_post(httpd_req_t *req)
{
    char body[300] = "";
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0)
            break;
        got += r;
    }
    body[got] = 0;
    char ssid[64], pass[100];
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value(body, "password", pass, sizeof(pass));
    httpd_resp_set_type(req, "text/html");
    if (!ssid[0])
        return httpd_resp_send(req, "No network name given. <a href='/'>Back</a>", HTTPD_RESP_USE_STRLEN);
    if (!sd_present() || !config_write_wifi(ssid, pass))
        return httpd_resp_send(req, "Could not write LEADIN.CFG: is a FAT32 SD card in the slot? "
                               "<a href='/'>Back</a>", HTTPD_RESP_USE_STRLEN);
    nlog("Setup: network '%s' saved, restarting", ssid);
    httpd_resp_send(req, "Saved. The board restarts and joins your network. Then open "
                    "http://leadin.local/", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ start */
static httpd_handle_t start_server(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = HTTP_PORT;
    c.stack_size = 8192;
    c.max_uri_handlers = 20;
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.recv_wait_timeout = 30;
    c.send_wait_timeout = 30;
    c.lru_purge_enable = true;
    c.core_id = 0;
    httpd_handle_t h = NULL;
    if (httpd_start(&h, &c) != ESP_OK) {
        nlog("HTTP server failed to start");
        return NULL;
    }
    return h;
}

static void reg(httpd_handle_t h, const char *uri, httpd_method_t m, esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t u = {.uri = uri, .method = m, .handler = fn};
    httpd_register_uri_handler(h, &u);
}

void webui_start(void)
{
    server = start_server();
    if (!server)
        return;
    reg(server, "/", HTTP_GET, h_index);
    reg(server, "/index.html", HTTP_GET, h_index);
    reg(server, "/api/state", HTTP_GET, h_state);
    reg(server, "/api/select", HTTP_POST, h_select);
    reg(server, "/api/audio", HTTP_POST, h_audio);
    reg(server, "/api/volume", HTTP_POST, h_volume);
    reg(server, "/api/export", HTTP_GET, h_export);
    reg(server, "/api/upload", HTTP_PUT, h_upload);
    reg(server, "/api/system", HTTP_POST, h_system);
    reg(server, "/api/firmware", HTTP_POST, h_firmware);
    nlog("Web UI on port %d", HTTP_PORT);
}

void setup_portal_start(void)
{
    server = start_server();
    if (!server)
        return;
    reg(server, "/setup", HTTP_POST, h_setup_post);
    reg(server, "/*", HTTP_GET, h_setup_page);
}
