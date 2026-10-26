/* Local Ethernet setup page. Based on Espressif's sta2eth example (CC0). */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "dns_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static httpd_handle_t s_server;
static EventGroupHandle_t *s_flags;
static int s_success_bit;
extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[] asm("_binary_portal_html_end");

static bool form_decode(const char *input, char *output, size_t capacity)
{
    size_t j = 0;
    for (size_t i = 0; input[i]; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            char a = input[++i];
            char b = a ? input[++i] : 0;
            if (!b) return false;
            int hi = (a >= '0' && a <= '9') ? a - '0' :
                     (a >= 'A' && a <= 'F') ? a - 'A' + 10 :
                     (a >= 'a' && a <= 'f') ? a - 'a' + 10 : -1;
            int lo = (b >= '0' && b <= '9') ? b - '0' :
                     (b >= 'A' && b <= 'F') ? b - 'A' + 10 :
                     (b >= 'a' && b <= 'f') ? b - 'a' + 10 : -1;
            if (hi < 0 || lo < 0) return false;
            c = (unsigned char)((hi << 4) | lo);
        }
        if (!c || j + 1 >= capacity) return false;
        output[j++] = c;
    }
    output[j] = 0;
    return true;
}

bool is_provisioned(void)
{
    wifi_config_t cfg = {0};
    return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != 0;
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, portal_html_start, portal_html_end - portal_html_start);
}

static esp_err_t portal_api_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/captive+json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req,
        "{\"captive\":true,\"user-portal-url\":\"http://192.168.44.1/\"}");
}

static esp_err_t networks_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"scan_failed\"}");
    }

    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"scan_failed\"}");
    }
    if (count == 0) return httpd_resp_sendstr(req, "[]");
    if (count > 40) count = 40;
    wifi_ap_record_t *records = calloc(count, sizeof(*records));
    if (!records) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        free(records);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"scan_failed\"}");
    }

    esp_err_t sent = httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    for (uint16_t i = 0; i < count && sent == ESP_OK; i++) {
        size_t len = strnlen((const char *)records[i].ssid, sizeof(records[i].ssid));
        if (!len) continue;
        bool duplicate = false;
        for (uint16_t j = 0; j < i; j++) {
            if (strnlen((const char *)records[j].ssid, sizeof(records[j].ssid)) == len &&
                memcmp(records[i].ssid, records[j].ssid, len) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        char item[120];
        size_t pos = 0;
        pos += snprintf(item + pos, sizeof(item) - pos, "%s{\"hex\":\"", first ? "" : ",");
        for (size_t j = 0; j < len; j++) {
            pos += snprintf(item + pos, sizeof(item) - pos, "%02X", records[i].ssid[j]);
        }
        pos += snprintf(item + pos, sizeof(item) - pos, "\",\"band\":\"%s\",\"rssi\":%d}",
                        records[i].primary <= 14 ? "2,4 GHz" : "5 GHz", records[i].rssi);
        sent = httpd_resp_send_chunk(req, item, pos);
        first = false;
    }
    free(records);
    if (sent == ESP_OK) sent = httpd_resp_send_chunk(req, "]", 1);
    if (sent == ESP_OK) sent = httpd_resp_send_chunk(req, NULL, 0);
    return sent;
}

static esp_err_t captive_redirect(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.44.1/");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, "<meta charset='utf-8'><a href='http://192.168.44.1/'>Set up Wifi2Eth</a>");
}

static esp_err_t connect_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 256) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid input");
    }
    char body[257] = {0};
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            memset(body, 0, sizeof(body));
            return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Upload interrupted");
        }
        received += n;
    }
    char encoded_ssid[257] = {0};
    char encoded_password[257] = {0};
    char ssid[33] = {0};
    char password[65] = {0};
    bool valid = httpd_query_key_value(body, "ssid", encoded_ssid, sizeof(encoded_ssid)) == ESP_OK &&
                 form_decode(encoded_ssid, ssid, sizeof(ssid)) && ssid[0] != 0;
    if (httpd_query_key_value(body, "password", encoded_password, sizeof(encoded_password)) == ESP_OK) {
        valid = valid && form_decode(encoded_password, password, sizeof(password));
    }
    char language[4] = {0};
    bool german = httpd_query_key_value(body, "lang", language, sizeof(language)) == ESP_OK &&
                  strcmp(language, "de") == 0;
    memset(body, 0, sizeof(body));
    memset(encoded_password, 0, sizeof(encoded_password));
    if (!valid || strlen(password) > 63 || (password[0] && strlen(password) < 8)) {
        memset(password, 0, sizeof(password));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SSID or password");
    }
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, strlen(ssid));
    memcpy(cfg.sta.password, password, strlen(password));
    esp_err_t err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    // Keep the setup AP alive until the HTTP response has been delivered.
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(password, 0, sizeof(password));
    memset(&cfg, 0, sizeof(cfg));
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save Wi-Fi settings");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t sent = httpd_resp_sendstr(req, german ?
        "<meta charset='utf-8'><h1>Gespeichert</h1><p>Bridge startet neu.</p>" :
        "<meta charset='utf-8'><h1>Saved</h1><p>The bridge is restarting.</p>");
    if (sent == ESP_OK && s_flags) xEventGroupSetBits(*s_flags, s_success_bit);
    return sent;
}

static esp_err_t firmware_post(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!target || req->content_len <= 0 || (size_t)req->content_len > target->size) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"size\"}");
    }

    uint8_t *buffer = malloc(4096);
    if (!buffer) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"memory\"}");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = ESP_OK;
    size_t remaining = req->content_len;
    bool started = false;
    while (remaining > 0) {
        int received = httpd_req_recv(req, (char *)buffer,
                                      remaining < 4096 ? remaining : 4096);
        if (received <= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (!started) {
            // Application images begin with the ESP image magic byte. This also
            // rejects merged factory images and multipart form uploads early.
            if (buffer[0] != 0xE9) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }
            err = esp_ota_begin(target, req->content_len, &handle);
            if (err != ESP_OK) break;
            started = true;
        }
        err = esp_ota_write(handle, buffer, received);
        if (err != ESP_OK) break;
        remaining -= received;
    }
    free(buffer);
    if (err == ESP_OK) err = esp_ota_end(handle);
    else if (started) esp_ota_abort(handle);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_image\"}");
    }
    esp_err_t sent = httpd_resp_sendstr(req, "{\"ok\":true}");
    if (sent == ESP_OK && s_flags) xEventGroupSetBits(*s_flags, s_success_bit);
    return sent;
}

static const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = page_get};
static const httpd_uri_t connect_uri = {.uri = "/connect", .method = HTTP_POST, .handler = connect_post};
static const httpd_uri_t networks_uri = {.uri = "/api/networks", .method = HTTP_GET, .handler = networks_get};
static const httpd_uri_t portal_uri = {.uri = "/captive-portal/api", .method = HTTP_GET, .handler = portal_api_get};
static const httpd_uri_t firmware_uri = {.uri = "/api/firmware", .method = HTTP_POST, .handler = firmware_post};

esp_err_t start_provisioning(EventGroupHandle_t *flags, int success_bit, int fail_bit)
{
    (void)fail_bit;
    s_flags = flags;
    s_success_bit = success_bit;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 30;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) return err;
    err = httpd_register_uri_handler(s_server, &root);
    if (err != ESP_OK) return err;
    err = httpd_register_uri_handler(s_server, &connect_uri);
    if (err != ESP_OK) return err;
    err = httpd_register_uri_handler(s_server, &networks_uri);
    if (err != ESP_OK) return err;
    err = httpd_register_uri_handler(s_server, &portal_uri);
    if (err != ESP_OK) return err;
    err = httpd_register_uri_handler(s_server, &firmware_uri);
    if (err != ESP_OK) return err;
    err = httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_redirect);
    if (err != ESP_OK) return err;
    dns_server_config_t dns = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    return start_dns_server(&dns) ? ESP_OK : ESP_FAIL;
}
