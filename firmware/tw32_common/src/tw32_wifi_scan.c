#include "tw32_wifi_scan.h"
#include "tw32_json_out.h"

#include <stdlib.h>
#include <string.h>

#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    SemaphoreHandle_t request;
    SemaphoreHandle_t response;
    wifi_ap_record_t *records;
    size_t            max_records;
    int               result;
} scan_chan_t;

static scan_chan_t       s_scan;
static SemaphoreHandle_t s_cache_lock;
static tw32_ap_entry_t   s_cache[TW32_WIFI_SCAN_MAX];
static int               s_cache_count;
static bool              s_inited;

static void mac_bytes_to_str(const uint8_t *mac, char *out, size_t out_len)
{
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Short label for the AP's authentication mode. WPA2-PSK is the only
 * mode that yields a PMKID; WPA3-SAE and OWE do not, so surfacing this
 * lets the operator pick a viable target before attacking. */
static const char *auth_str(wifi_auth_mode_t m)
{
    switch (m) {
        case WIFI_AUTH_OPEN:          return "open";
        case WIFI_AUTH_WEP:           return "wep";
        case WIFI_AUTH_WPA_PSK:       return "wpa";
        case WIFI_AUTH_WPA2_PSK:      return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "wpa/wpa2";
        case WIFI_AUTH_WPA3_PSK:      return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
        case WIFI_AUTH_OWE:           return "owe";
        default:                      return "other";
    }
}

static void scan_worker_task(void *arg)
{
    (void)arg;
    while (true) {
        xSemaphoreTake(s_scan.request, portMAX_DELAY);
        wifi_scan_config_t cfg = {
            .ssid = NULL, .bssid = NULL, .channel = 0,
            .show_hidden = true,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time = { .active = { .min = 100, .max = 300 } },
        };
        int rc = -1;
        if (esp_wifi_scan_start(&cfg, true /* block */) == ESP_OK) {
            uint16_t n = 0;
            if (esp_wifi_scan_get_ap_num(&n) == ESP_OK) {
                if (n > s_scan.max_records) n = (uint16_t)s_scan.max_records;
                if (n == 0) {
                    rc = 0;
                } else if (esp_wifi_scan_get_ap_records(&n, s_scan.records) == ESP_OK) {
                    rc = (int)n;
                }
            }
        }
        s_scan.result = rc;
        xSemaphoreGive(s_scan.response);
    }
}

void tw32_wifi_scan_init(void)
{
    if (s_inited) return;
    s_inited = true;
    s_scan.request  = xSemaphoreCreateBinary();
    s_scan.response = xSemaphoreCreateBinary();
    s_cache_lock    = xSemaphoreCreateMutex();
    /* 6 KB stack; esp_wifi_scan_start has been observed to overflow 4 KB. */
    xTaskCreatePinnedToCore(scan_worker_task, "tw32-scan", 6144, NULL, 5, NULL, 1);
}

static int scan_blocking_internal(wifi_ap_record_t *records, size_t max)
{
    /* Drain any stale response. */
    xSemaphoreTake(s_scan.response, 0);
    s_scan.records     = records;
    s_scan.max_records = max;
    s_scan.result      = -1;
    xSemaphoreGive(s_scan.request);
    if (xSemaphoreTake(s_scan.response, pdMS_TO_TICKS(15000)) != pdTRUE) {
        return -1;
    }
    return s_scan.result;
}

int tw32_wifi_scan_run(void)
{
    if (!s_inited) return -1;

    /* If promiscuous is on, disable it for the scan and restore after.
     * Promiscuous + scan is supported by IDF, but disabling avoids
     * spurious RX during the channel sweep. */
    bool promisc_was_on = false;
    esp_wifi_get_promiscuous(&promisc_was_on);
    if (promisc_was_on) esp_wifi_set_promiscuous(false);

    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * TW32_WIFI_SCAN_MAX);
    if (!records) {
        if (promisc_was_on) esp_wifi_set_promiscuous(true);
        return -1;
    }

    int count = scan_blocking_internal(records, TW32_WIFI_SCAN_MAX);

    if (promisc_was_on) esp_wifi_set_promiscuous(true);

    if (count < 0) {
        free(records);
        return -1;
    }

    /* Update cache atomically. */
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    s_cache_count = count;
    for (int i = 0; i < count; i++) {
        memcpy(s_cache[i].bssid, records[i].bssid, 6);
        s_cache[i].channel  = records[i].primary;
        s_cache[i].rssi     = records[i].rssi;
        s_cache[i].authmode = records[i].authmode;
        strncpy(s_cache[i].ssid, (char *)records[i].ssid, TW32_WIFI_SSID_MAX);
        s_cache[i].ssid[TW32_WIFI_SSID_MAX] = '\0';
    }
    xSemaphoreGive(s_cache_lock);

    /* Emit one event:"ap" per AP with 1-based idx (matches wifi-deauth's
     * historical contract). */
    for (int i = 0; i < count; i++) {
        char bssid_str[18];
        mac_bytes_to_str(records[i].bssid, bssid_str, sizeof(bssid_str));
        tw32_json_begin();
        tw32_json_kv_str ("event", "ap");
        tw32_json_kv_int ("idx",     i + 1);
        tw32_json_kv_str ("ssid",    (char *)records[i].ssid);
        tw32_json_kv_str ("bssid",   bssid_str);
        tw32_json_kv_int ("channel", records[i].primary);
        tw32_json_kv_int ("rssi",    records[i].rssi);
        tw32_json_kv_str ("auth",    auth_str(records[i].authmode));
        tw32_json_end();
    }
    /* Terminator. */
    tw32_json_begin();
    tw32_json_kv_str ("cmd",       "scan");
    tw32_json_kv_bool("ok",        true);
    tw32_json_kv_int ("count",     count);
    tw32_json_kv_bool("truncated", count >= TW32_WIFI_SCAN_MAX);
    tw32_json_end();

    free(records);
    return count;
}

bool tw32_wifi_scan_resolve_index(const char *s, tw32_ap_entry_t *out)
{
    if (!s || !*s) return false;
    char *end;
    long n = strtol(s, &end, 10);
    if (*end != '\0') return false;
    bool ok = false;
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    if (n >= 1 && n <= s_cache_count) {
        *out = s_cache[n - 1];
        ok = true;
    }
    xSemaphoreGive(s_cache_lock);
    return ok;
}

int tw32_wifi_scan_silent(wifi_ap_record_t *records, size_t max_records)
{
    if (!s_inited || records == NULL || max_records == 0) return -1;
    return scan_blocking_internal(records, max_records);
}

int tw32_wifi_scan_cache_count(void)
{
    if (!s_inited) return 0;
    int n;
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    n = s_cache_count;
    xSemaphoreGive(s_cache_lock);
    return n;
}
