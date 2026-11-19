/* wired_openeth.c - the OpenCores Ethernet that QEMU emulates
 *
 * Needs CONFIG_ETH_USE_OPENETH=y (boards/qemu/sdkconfig.board).
 */
#include "sdkconfig.h"
#include "hal_drivers.h"

#ifdef CONFIG_ETH_USE_OPENETH
#include "esp_eth.h"

static bool start(const char *hostname, esp_netif_t **out)
{
    esp_netif_config_t nc = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&nc);
    esp_netif_set_hostname(netif, hostname);
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_cfg);
    esp_eth_config_t ec = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth;
    if (esp_eth_driver_install(&ec, &eth) != ESP_OK)
        return false;
    esp_netif_attach(netif, esp_eth_new_netif_glue(eth));
    *out = netif;
    return esp_eth_start(eth) == ESP_OK;
}

static const hal_wired_t part = {.name = "Ethernet (QEMU)", .start = start};

const hal_wired_t *wired_openeth(void)
{
    return &part;
}
#endif
