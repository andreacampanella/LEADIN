/* ota.c - firmware update from the web UI (POST /api/firmware, the raw .bin)
 *
 * The upload only works while the switch in the web UI (g_set.ota_enabled)
 * is on. The switch is off after every start. The image must come from this
 * project and must be built for this board: the first part of the upload
 * holds the app description, with the project name and the version string
 * "<version>-<board>" (set in CMakeLists.txt).
 *
 * The new image goes to the other OTA slot. The bootloader keeps the old
 * image until the new one marks itself valid (ota_mark_valid, called once
 * the network and the card are up). If the new image fails before that,
 * the next reset rolls back.
 */
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leadin.h"

/* "ota_0" or "ota_1 (pending verify)": which image runs, for the log. */
const char *ota_running(void)
{
    static char text[40];
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    const char *state = "";
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY)
        state = " (pending verify)";
    snprintf(text, sizeof(text), "image %s%s", run->label, state);
    return text;
}

void ota_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        nlog("Firmware update confirmed (%s)", run->label);
    }
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t fail_status(httpd_req_t *req, const char *status, const char *msg)
{
    nlog("Firmware update refused: %s", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char b[160];
    snprintf(b, sizeof(b), "{\"error\":\"%s\"}", msg);
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t fail(httpd_req_t *req, const char *msg)
{
    return fail_status(req, "400 Bad Request", msg);
}

/* The app description sits after the image header and the first segment
 * header. Compare the project name and the board part of the version. */
#define APP_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

static const char *check_image(const uint8_t *head, int len)
{
    if (len < (int)(APP_DESC_OFFSET + sizeof(esp_app_desc_t)))
        return "the file is too short";
    esp_app_desc_t d;
    memcpy(&d, head + APP_DESC_OFFSET, sizeof(d));
    if (d.magic_word != ESP_APP_DESC_MAGIC_WORD)
        return "the file is not an ESP32 firmware image";
    const esp_app_desc_t *me = esp_app_get_description();
    if (strncmp(d.project_name, me->project_name, sizeof(d.project_name)))
        return "the file is not a LEADIN firmware image";
    const char *my_board = strchr(me->version, '-');
    const char *its_board = strchr(d.version, '-');
    if (!my_board || !its_board || strcmp(my_board, its_board))
        return "the image is built for another board";
    return NULL;
}

int ota_upload_handler(httpd_req_t *req)
{
    if (!g_set.ota_enabled)
        return fail_status(req, "403 Forbidden", "the firmware update switch is off");
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part)
        return fail(req, "no OTA partition");
    if (req->content_len <= 0 || req->content_len > (int)part->size)
        return fail(req, "the file does not fit in the OTA partition");
    esp_ota_handle_t h;
    if (esp_ota_begin(part, req->content_len, &h) != ESP_OK)
        return fail(req, "cannot erase the OTA partition");
    audio_reset();                      /* the flash writes pause the cache */
    nlog("Firmware update: %d bytes to %s", req->content_len, part->label);
    char *buf = malloc(4096);
    int left = req->content_len;
    bool first = true;
    const char *why = NULL;
    esp_err_t err = buf ? ESP_OK : ESP_ERR_NO_MEM;
    while (err == ESP_OK && left > 0) {
        int want = left < 4096 ? left : 4096;
        int got = 0;
        while (got < want) {            /* fill the chunk: the check needs the whole head */
            int r = httpd_req_recv(req, buf + got, want - got);
            if (r == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            if (r <= 0) {
                err = ESP_FAIL;
                break;
            }
            got += r;
        }
        if (err != ESP_OK)
            break;
        if (first) {
            first = false;
            why = check_image((uint8_t *)buf, got);
            if (why)
                break;
        }
        err = esp_ota_write(h, buf, got);
        left -= got;
    }
    free(buf);
    if (why) {
        esp_ota_abort(h);
        return fail(req, why);
    }
    if (err != ESP_OK) {
        esp_ota_abort(h);
        return fail(req, "upload interrupted or write error");
    }
    if (esp_ota_end(h) != ESP_OK)
        return fail(req, "the file is not a valid firmware image");
    if (esp_ota_set_boot_partition(part) != ESP_OK)
        return fail(req, "cannot select the new image");
    g_set.ota_enabled = false;
    nlog("Firmware update written, restarting");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"restart\":true}", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}
