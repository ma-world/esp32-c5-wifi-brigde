/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include <stdio.h>
#include <esp_timer.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_private/wifi.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "lwip/ip4_addr.h"
#include "provisioning.h"
#include "wired_iface.h"

static const char *TAG = "example_sta2wired";

static EventGroupHandle_t s_event_flags;
static bool s_wifi_is_connected = false;
static uint8_t s_sta_mac[6];
static const char s_portal_url[] = "http://192.168.44.1/captive-portal/api";

static bool is_dhcp_frame(const void *buffer, uint16_t len, bool from_client)
{
    const uint8_t *frame = buffer;
    if (len < 14 + 20 + 8 || frame[12] != 0x08 || frame[13] != 0x00) return false;
    const uint8_t *ip = frame + 14;
    unsigned header_len = (ip[0] & 0x0f) * 4;
    if ((ip[0] >> 4) != 4 || header_len < 20 || len < 14 + header_len + 8 || ip[9] != 17) return false;
    const uint8_t *udp = ip + header_len;
    return from_client ? (udp[0] == 0 && udp[1] == 68 && udp[2] == 0 && udp[3] == 67)
                       : (udp[0] == 0 && udp[1] == 67 && udp[2] == 0 && udp[3] == 68);
}

static esp_err_t start_setup_hotspot(void)
{
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (!ap) return ESP_FAIL;
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap));
    esp_netif_ip_info_t info = {0};
    IP4_ADDR(&info.ip, 192, 168, 44, 1);
    IP4_ADDR(&info.gw, 192, 168, 44, 1);
    IP4_ADDR(&info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap, &info));
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4 = info.ip;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                           (void *)s_portal_url, strlen(s_portal_url)));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap));

    static const char password[] = "esp32_eth";
    wifi_config_t wifi = {0};
    snprintf((char *)wifi.ap.ssid, sizeof(wifi.ap.ssid), "Wifi2Eth_%02X%02X%02X",
             s_sta_mac[3], s_sta_mac[4], s_sta_mac[5]);
    wifi.ap.ssid_len = strlen((char *)wifi.ap.ssid);
    memcpy(wifi.ap.password, password, sizeof(password));
    wifi.ap.channel = 1;
    wifi.ap.max_connection = 2;
    wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
    ESP_LOGW(TAG, "Setup hotspot: %s / password: %s / http://192.168.44.1/",
             wifi.ap.ssid, password);
    memset(&wifi, 0, sizeof(wifi));
    return ESP_OK;
}

const int CONNECTED_BIT = BIT0;
const int DISCONNECTED_BIT = BIT1;
const int RECONFIGURE_BIT = BIT2;
const int PROV_SUCCESS_BIT = BIT3;
const int PROV_FAIL_BIT = BIT4;

/**
 * WiFi -- Wired packet path
 */
static esp_err_t wired_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    bool dhcp = is_dhcp_frame(buffer, len, true);
    if (dhcp) ESP_LOGI(TAG, "DHCP request received from Ethernet");
    if (s_wifi_is_connected) {
        mac_spoof(FROM_WIRED, buffer, len, s_sta_mac);
        if (dhcp) ESP_LOGI(TAG, "DHCP source MAC rewritten: %s",
                           memcmp((uint8_t *)buffer + 6, s_sta_mac, 6) == 0 ? "yes" : "no");
        esp_err_t err = esp_wifi_internal_tx(ESP_IF_WIFI_STA, buffer, len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi transmit failed: %s", esp_err_to_name(err));
        }
        if (dhcp) ESP_LOGI(TAG, "DHCP request Wi-Fi transmit: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static void wifi_buff_free(void *buffer, void *ctx)
{
    esp_wifi_internal_free_rx_buffer(buffer);
}

static esp_err_t wifi_recv_callback(void *buffer, uint16_t len, void *eb)
{
    static unsigned received = 0;
    if (received++ < 10) ESP_LOGI(TAG, "Wi-Fi frame received: ethertype=%02x%02x, length=%u",
                                  len >= 14 ? ((uint8_t *)buffer)[12] : 0,
                                  len >= 14 ? ((uint8_t *)buffer)[13] : 0, len);
    if (is_dhcp_frame(buffer, len, false)) ESP_LOGI(TAG, "DHCP reply received from Wi-Fi");
    mac_spoof(TO_WIRED, buffer, len, s_sta_mac);
    if (wired_send(buffer, len, eb) != ESP_OK) {
        esp_wifi_internal_free_rx_buffer(eb);
        ESP_LOGD(TAG, "Failed to send packet to USB!");
    }
    return ESP_OK;
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi STA disconnected");
        s_wifi_is_connected = false;
        esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, NULL);
        esp_wifi_connect();

        xEventGroupClearBits(s_event_flags, CONNECTED_BIT);
        xEventGroupSetBits(s_event_flags, DISCONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi STA connected");
        esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, wifi_recv_callback);
        s_wifi_is_connected = true;
        xEventGroupClearBits(s_event_flags, DISCONNECTED_BIT);
        xEventGroupSetBits(s_event_flags, CONNECTED_BIT);
    }
}

static esp_err_t connect_wifi(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_start() );
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_config_t wifi_cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK) {
        // configuration not available, report error to restart provisioning
        return ESP_FAIL;
    }
    esp_wifi_connect();
    EventBits_t status = xEventGroupWaitBits(s_event_flags, CONNECTED_BIT, 0, 1, 10000 / portTICK_PERIOD_MS);
    if (status & CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi station connected successfully");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WiFi station connected failed");
    return ESP_ERR_TIMEOUT;
}

/**
 * GPIO button functionality
 */
#define GPIO_INPUT          CONFIG_EXAMPLE_RECONFIGURE_BUTTON
#define GPIO_LONG_PUSH_US   2000000  /* push for 2 seconds to reconfigure */

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    static int64_t last_pushed = -1;
    if (gpio_get_level(GPIO_INPUT) == 0) {
        last_pushed = esp_timer_get_time();
    } else {
        uint64_t now = esp_timer_get_time();
        if (last_pushed != -1 && now - last_pushed > GPIO_LONG_PUSH_US) {
            BaseType_t high_task_wakeup;
            xEventGroupSetBitsFromISR(s_event_flags, RECONFIGURE_BIT, &high_task_wakeup);
            if (high_task_wakeup) {
                portYIELD_FROM_ISR();
            }
        }
        last_pushed = -1;
    }
}

static void gpio_init(void)
{
    gpio_config_t io_conf = { .intr_type = GPIO_INTR_ANYEDGE,
                              .pin_bit_mask = (1ULL << GPIO_INPUT),
                              .mode = GPIO_MODE_INPUT,
                              .pull_up_en = 1
                            };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT, gpio_isr_handler, NULL);
}

/**
 * Application
 */
void app_main(void)
{
    static __NOINIT_ATTR uint32_t s_reconfigure_requested;
    static const uint32_t RECONFIGURE_REQUEST = 0x1C55AA;

    /* Check reset reason and decide if we should re-provision */
    bool do_provision = false;
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGD(TAG, "After restart! %d", reason);
    if (reason != ESP_RST_SW) {
        s_reconfigure_requested = 0;
    } else if (s_reconfigure_requested == RECONFIGURE_REQUEST) {
        do_provision = true;
    }

    /* Initialize NVS and WiFi */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // init the flags and event loop
    s_event_flags = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Init the re-provisioning button (long-press with initiate provisioning restart) */
    gpio_init();
    esp_read_mac(s_sta_mac, ESP_MAC_WIFI_STA);

    /* Start the application in configuration mode (to perform provisioning)
     * or in a bridge mode (already provisioned) */
    bool bridge_mode = false;
    if (do_provision || !is_provisioned()) {
        ESP_LOGI(TAG, "Starting provisioning");
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(start_setup_hotspot());
        ESP_ERROR_CHECK(start_provisioning(&s_event_flags, PROV_SUCCESS_BIT, PROV_FAIL_BIT));
    } else {
        ESP_LOGI(TAG, "Starting Wi-Fi-to-Ethernet bridge");
        if (connect_wifi() != ESP_OK) {
            // if we cannot connect to WiFi we just try to re-provision
            xEventGroupSetBits(s_event_flags, RECONFIGURE_BIT);
        } else {
            // start the wired interface in the bridge mode
            wired_bridge_init(wired_recv_callback, wifi_buff_free);
            bridge_mode = true;
        }
    }

    EventBits_t bits;
    for (;;) {
        bits = xEventGroupWaitBits(s_event_flags, RECONFIGURE_BIT | PROV_SUCCESS_BIT | PROV_FAIL_BIT,
                                   pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));
        // xEventGroupWaitBits returns all currently set bits, including the
        // persistent CONNECTED_BIT. Only the requested actions end this loop.
        if (bits & (RECONFIGURE_BIT | PROV_SUCCESS_BIT | PROV_FAIL_BIT)) break;
        // Return to the local setup page after a sustained hotel Wi-Fi outage.
        if (bridge_mode && !s_wifi_is_connected) {
            bits = RECONFIGURE_BIT;
            break;
        }
    }
    if (bits & RECONFIGURE_BIT || bits & PROV_FAIL_BIT) {
        // retry provisioning if it previously failed or if requested by the button press
        s_reconfigure_requested = RECONFIGURE_REQUEST;
    } else {
        // provisioning successfully finished, restart to the bridge mode
        s_reconfigure_requested = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));    // to let httpd handle the closure
    esp_restart();
}
