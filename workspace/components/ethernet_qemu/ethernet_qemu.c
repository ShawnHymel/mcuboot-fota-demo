/*
 * SPDX-FileCopyrightText: 2025 Shawn Hymel
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "ethernet_qemu.h"

// Tag for debug messages
static const char *TAG = "eth";

// Static global variables
static esp_eth_handle_t s_eth_handle = NULL;
static esp_eth_phy_t *s_eth_phy = NULL;
static esp_eth_mac_t *s_eth_mac = NULL;
static esp_netif_t *s_eth_netif = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;

/*******************************************************************************
 * Private function prototypes
 */

static void on_eth_event(void *arg, 
                         esp_event_base_t event_base, 
                         int32_t event_id, 
                         void *event_data);

static void on_got_ip_event(void *arg, 
                            esp_event_base_t event_base, 
                            int32_t event_id, 
                            void *event_data);

/*******************************************************************************
 * Private function definitions
 */

// Event handler: Ethernet events
static void on_eth_event(void *arg, 
                         esp_event_base_t event_base, 
                         int32_t event_id, 
                         void *event_data)
{
    // Determine event type
    switch (event_id) {

        // Get MAC address when connected and print it
        case ETHERNET_EVENT_CONNECTED:
            esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
            uint8_t mac_addr[6] = {0};
            esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
            ESP_LOGI(TAG, "Ethernet link up");
            ESP_LOGI(TAG, 
                     "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                     mac_addr[0], 
                     mac_addr[1], 
                     mac_addr[2], 
                     mac_addr[3], 
                     mac_addr[4], 
                     mac_addr[5]);
            break;
        
        // Print message when disconnected
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet disconnected. Attempting to reconnect...");
            eth_qemu_reconnect();
            break;

        // Print message when started
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet started");
            break;

        // Print message when stopped
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet stopped");
            break;

        // Default case: do nothing
        default:
            ESP_LOGI(TAG, "Unhandled Ethernet event");
            break;
    }
}

// Event handler: received IP address on Ethernet interface from DHCP
static void on_got_ip_event(void *arg, 
                            esp_event_base_t event_base, 
                            int32_t event_id, 
                            void *event_data)
{

    // Determine event type
    switch (event_id) {

        // Print IP address
        case IP_EVENT_ETH_GOT_IP:
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            esp_netif_ip_info_t *ip_info = &event->ip_info;
            ESP_LOGI(TAG, "Ethernet IP address obtained");
            ESP_LOGI(TAG, "  IP address:" IPSTR, IP2STR(&ip_info->ip));
            ESP_LOGI(TAG, "  Netmask:" IPSTR, IP2STR(&ip_info->netmask));
            ESP_LOGI(TAG, "  Gateway:" IPSTR, IP2STR(&ip_info->gw));
            break;

        // Notify if lost IP address
        case IP_EVENT_ETH_LOST_IP:
            ESP_LOGI(TAG, "Ethernet lost IP address");
            break;

        // Default case: do nothing
        default:
            ESP_LOGI(TAG, "Unhandled IP event");
            break;
    }
}

/*******************************************************************************
 * Public functions
 */

// Check if Ethernet is up/connected
bool eth_qemu_is_connected(void)
{
    return esp_netif_is_netif_up(s_eth_netif);
}

// Check if we have an IP address on the Ethernet interface
bool eth_qemu_has_ip_addr(void)
{
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(s_eth_netif, &ip_info);
    return ip_info.ip.addr != 0;
}

// Initialize QEMU virtual Ethernet
esp_err_t eth_qemu_init(void)
{
    esp_err_t esp_ret;

    // Print message
    ESP_LOGI(TAG, "Starting Ethernet...");

    // Initialize network interface for Ethernet
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_config);
    if (!s_eth_netif) {
        ESP_LOGE(TAG, "Failed to create Ethernet interface");
        return ESP_FAIL;
    }

    // Configure physical layer (PHY) and create PHY instance
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 100;
    s_eth_phy = esp_eth_phy_new_dp83848(&phy_config);
    if (!s_eth_phy) {
        ESP_LOGE(TAG, "Failed to create PHY instance");
        return ESP_FAIL;
    }

    // Configure media access control (MAC) layer and create MAC instance
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    s_eth_mac = esp_eth_mac_new_openeth(&mac_config);
    if (!s_eth_mac) {
        ESP_LOGE(TAG, "Failed to create MAC instance");
        return ESP_FAIL;
    }

    // Initialize Ethernet driver, connect MAC and PHY
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(s_eth_mac, s_eth_phy);
    esp_ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Ethernet driver");
        return esp_ret;
    }

    // Create glue layer to attach Ethernet driver to network interface
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (!s_eth_glue) {
        ESP_LOGE(TAG, "Failed to create glue layer");
        return ESP_FAIL;
    }

    // Attach Ethernet driver to network interface
    esp_ret = esp_netif_attach(s_eth_netif, s_eth_glue);
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach Ethernet driver to network interface");
        return esp_ret;
    }

    // Register Ethernet event handler
    esp_ret = esp_event_handler_register(ETH_EVENT, 
                                         ESP_EVENT_ANY_ID, 
                                         &on_eth_event, 
                                         NULL);
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register Ethernet event handler");
        return esp_ret;
    }

    // Register IP event handler
    esp_ret = esp_event_handler_register(IP_EVENT, 
                                         IP_EVENT_ETH_GOT_IP, 
                                         &on_got_ip_event, 
                                         NULL);
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler");
        return esp_ret;
    }

    // Start Ethernet driver
    esp_ret = esp_eth_start(s_eth_handle);
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Ethernet driver");
        return esp_ret;
    }

    return ESP_OK;
}

esp_err_t eth_qemu_stop(void)
{
    esp_err_t esp_ret;

    // Print message
    ESP_LOGI(TAG, "Stopping Ethernet...");

    // Unregister Ethernet event handlers
    esp_ret = esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event);
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unregister Ethernet event handler");
        return esp_ret;
    }

    // Unregister IP event handler
    esp_ret = esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip_event);
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unregister IP event handler");
        return esp_ret;
    }

    // Stop Ethernet driver
    if (s_eth_handle != NULL) {
        esp_ret = esp_eth_stop(s_eth_handle);
        if (esp_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop Ethernet driver");
            return esp_ret;
        }
    }

    // Delete glue layer
    if (s_eth_glue != NULL) {
        esp_ret = esp_eth_del_netif_glue(s_eth_glue);
        if (esp_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete glue layer");
            return esp_ret;
        }
    }

    // Uninstall Ethernet driver
    if (s_eth_handle != NULL) {
        esp_ret = esp_eth_driver_uninstall(s_eth_handle);
        if (esp_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to uninstall Ethernet driver");
            return esp_ret;
        }
    }

    // Delete PHY instance
    if (s_eth_phy != NULL) {
        esp_ret = s_eth_phy->del(s_eth_phy);
        if (esp_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete PHY instance");
            return esp_ret;
        }
    }

    // Delete MAC instance
    if (s_eth_mac != NULL) {
        esp_ret = s_eth_mac->del(s_eth_mac);
        if (esp_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete MAC instance");
            return esp_ret;
        }
    }

    // Destroy network interface
    if (s_eth_netif != NULL) {
        esp_netif_destroy(s_eth_netif);
    }

    // Set handles to NULL
    s_eth_handle = NULL;
    s_eth_phy = NULL;
    s_eth_mac = NULL;
    s_eth_netif = NULL;
    s_eth_glue = NULL;

    // Print message
    ESP_LOGI(TAG, "Ethernet stopped");

    return ESP_OK;
}

// Attempt reconnection to Ethernet
esp_err_t eth_qemu_reconnect(void)
{
    esp_err_t esp_ret;

    // Stop Ethernet
    esp_ret = eth_qemu_stop();
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop Ethernet");
        return esp_ret;
    }

    // Initialize Ethernet
    esp_ret = eth_qemu_init();
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Ethernet");
        return esp_ret;
    }

    return ESP_OK;
}