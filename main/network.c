/* network.c - the network
 *
 * Wi-Fi: station with SSID and password from LEADIN.CFG. If the file has no
 * network, or no connection comes up within WIFI_CONNECT_TIMEOUT_S, the
 * board starts an open access point LEADIN-SETUP with a setup page.
 * Wired: if the board has a wired interface (g_board.wired), it is used
 * instead of Wi-Fi, with DHCP.
 * Both: mDNS name <hostname>.local, and the time from SNTP for the log.
 */
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "leadin.h"

static esp_netif_t *netif;
static volatile bool up, setup_mode;
static char ip[16] = "0.0.0.0";

bool net_is_up(void) { return up; }
bool net_setup_mode(void) { return setup_mode; }
const char *net_ip(void) { return ip; }

int net_rssi(void)
{
#ifdef CONFIG_LEADIN_WIFI
    wifi_ap_record_t ap;
    if (g_board.wired || !up || setup_mode || esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
        return 0;
    return ap.rssi;
#else
    return 0;
#endif
}

static void start_services(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(g_cfg.hostname);
        mdns_instance_name_set("LEADIN server");
        mdns_service_add(NULL, "_http", "_tcp", HTTP_PORT, NULL, 0);
    }
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sc);
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = data;
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
    up = true;
    nlog("Network up: %s (http://%s.local/)", ip, g_cfg.hostname);
    start_services();
}

static void wired_start(void)
{
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL);
    if (g_board.wired->start(g_cfg.hostname, &netif))
        nlog("Network: %s", g_board.wired->name);
    else
        nlog("Network: %s did not start", g_board.wired->name);
}

#ifdef CONFIG_LEADIN_WIFI
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (up)
            nlog("Wi-Fi lost, reconnecting");
        up = false;
        if (!setup_mode)
            esp_wifi_connect();
    }
}

static void start_setup_ap(void)
{
    setup_mode = true;
    up = false;
    esp_wifi_stop();
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {
        .ap = {
            .ssid = SETUP_AP_SSID,
            .ssid_len = strlen(SETUP_AP_SSID),
            .channel = 6,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    strlcpy(ip, "192.168.4.1", sizeof(ip));
    nlog("Setup mode: join the Wi-Fi network %s and open http://192.168.4.1/", SETUP_AP_SSID);
    setup_portal_start();
}

#endif

void net_start(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    if (g_board.wired) {
        wired_start();
        return;
    }
#ifndef CONFIG_LEADIN_WIFI
    nlog("Network: this build has no Wi-Fi and the board has no wired network");
#else
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (!g_cfg.have_wifi) {
        nlog("No Wi-Fi network in LEADIN.CFG");
        start_setup_ap();
        return;
    }
    netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, g_cfg.hostname);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL);
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, g_cfg.ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, g_cfg.password, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = g_cfg.password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);      /* power save adds latency to every request */
    nlog("Wi-Fi: connecting to '%s'", g_cfg.ssid);
    for (int i = 0; i < WIFI_CONNECT_TIMEOUT_S * 10 && !up; i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (!up) {
        nlog("Wi-Fi: no connection to '%s' after %d s", g_cfg.ssid, WIFI_CONNECT_TIMEOUT_S);
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi);
        start_setup_ap();
    }
#endif
}
