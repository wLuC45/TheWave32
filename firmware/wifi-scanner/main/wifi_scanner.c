/*
 * TheWave32 / wifi-scanner
 *
 * Channel-hopping passive Wi-Fi AP scanner. Boots, takes over UART0
 * (the channel exposed via the CH343 USB-UART bridge on the dev
 * board), waits for `start`, then loops:
 *   - active scan across channels 1..13
 *   - emit one JSON line per AP discovered in the sweep
 *   - sleep `interval_ms` (default 1000)
 *
 * The Wi-Fi driver runs on core 0; scan results are formatted and
 * shipped on core 1 (via the CLI task and our scan task) to keep RF
 * timing tight.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"

#define MODULE_NAME "wifi-scanner"
#define MODULE_VERSION "0.2.0"

static const char *TAG = "wifi-scanner";

/* Runtime state shared between CLI handlers and the scan task. The task
 * is the sole writer of the counters; the CLI only reads them. They are
 * atomic so that cross-task read/RMW is well defined under the C memory
 * model rather than a (benign-on-Xtensa but undefined) data race. */
/* All cross-task fields are C11 atomics. `interval_ms` and `running`
 * are read by the scanner task in tight loops while the CLI task may
 * write to them at any moment; making them `_Atomic` removes the
 * benign-on-Xtensa-but-undefined data race the previous `volatile`-only
 * declarations had under the C11 memory model. */
typedef struct {
    _Atomic bool     running;
    _Atomic uint32_t interval_ms;
    _Atomic uint32_t scans_done;
    _Atomic uint32_t aps_seen;
} scanner_state_t;

static scanner_state_t s_state = {
    .running = false,
    .interval_ms = 1000,
    .scans_done = 0,
    .aps_seen = 0,
};

/* Hard cap on records pulled out of the Wi-Fi driver per sweep. The
 * driver itself may have discovered more APs than this in a dense
 * environment; we surface the truth to the host via a `scan_summary`
 * event so the operator can tell when we are saturating. */
enum { SCAN_RECORDS_CAP = 32 };

/* Maps the full ESP-IDF wifi_auth_mode_t (esp_wifi_types_generic.h) to
 * a short host-facing string. Source: ESP-IDF v5.x
 * components/esp_wifi/include/esp_wifi_types_generic.h; mode set as of
 * 802.11-2020 and WFA WPA3 / OWE / DPP additions. WIFI_AUTH_ENTERPRISE
 * and WIFI_AUTH_WPA2_ENTERPRISE alias to the same numeric value, so
 * only one of them appears in the switch. */
static const char *auth_to_str(wifi_auth_mode_t a)
{
    switch (a) {
        case WIFI_AUTH_OPEN:                return "open";
        case WIFI_AUTH_WEP:                 return "wep";
        case WIFI_AUTH_WPA_PSK:             return "wpa";
        case WIFI_AUTH_WPA2_PSK:            return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK:        return "wpa/wpa2";
        case WIFI_AUTH_WPA2_ENTERPRISE:     return "wpa2-eap";
        case WIFI_AUTH_WPA3_PSK:            return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK:       return "wpa2/wpa3";
        case WIFI_AUTH_WAPI_PSK:            return "wapi";
        case WIFI_AUTH_OWE:                 return "owe";
        case WIFI_AUTH_WPA3_ENT_192:        return "wpa3-eap-192";
        case WIFI_AUTH_WPA3_EXT_PSK:        return "wpa3";
        case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return "wpa3";
        case WIFI_AUTH_DPP:                 return "dpp";
        case WIFI_AUTH_WPA3_ENTERPRISE:     return "wpa3-eap";
        case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "wpa2/wpa3-eap";
        default:                            return "unknown";
    }
}

static void emit_ap(const wifi_ap_record_t *ap, uint32_t ts_ms)
{
    tw32_json_begin();
    tw32_json_kv_str("event", "ap");
    tw32_json_kv_uint("ts", ts_ms);
    tw32_json_kv_mac("bssid", ap->bssid);
    tw32_json_kv_str("ssid", (const char *)ap->ssid);
    tw32_json_kv_int("ch", ap->primary);
    tw32_json_kv_int("rssi", ap->rssi);
    tw32_json_kv_str("auth", auth_to_str(ap->authmode));
    tw32_json_end();
}

static void scan_once(void)
{
    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,                      /* 0 = all channels */
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 80,
        .scan_time.active.max = 120,
    };
    /* Blocking scan: returns ESP_ERR_WIFI_STATE if `cmd_stop` aborted
     * us via esp_wifi_scan_stop(). Either way the IDF driver has
     * already freed the in-flight record list, so we can just exit. */
    esp_err_t r = esp_wifi_scan_start(&cfg, true /*block*/);
    if (r != ESP_OK) {
        return;
    }
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        s_state.scans_done++;
        return;
    }

    /*
     * Records buffer lives in static storage so we don't blow the
     * task stack; wifi_ap_record_t is ~80 B, 32 of them is ~2.5 KB.
     * Static is fine because scan_once is only called from the
     * single scanner task. */
    static wifi_ap_record_t records[SCAN_RECORDS_CAP];
    uint16_t got = (count < SCAN_RECORDS_CAP) ? count : SCAN_RECORDS_CAP;
    /* esp_wifi_scan_get_ap_records frees the driver-side list whether
     * it returns ESP_OK or not (per IDF docs), so no extra cleanup. */
    if (esp_wifi_scan_get_ap_records(&got, records) != ESP_OK) {
        s_state.scans_done++;
        return;
    }

    uint32_t ts = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    for (uint16_t i = 0; i < got; ++i) {
        emit_ap(&records[i], ts);
    }

    /* Tell the host how many APs the driver found vs how many we
     * actually shipped. `total > got` means SCAN_RECORDS_CAP truncated
     * the sweep and the operator is in a dense RF environment. */
    tw32_json_begin();
    tw32_json_kv_str("event", "scan_summary");
    tw32_json_kv_uint("ts", ts);
    tw32_json_kv_uint("total", count);
    tw32_json_kv_uint("got", got);
    tw32_json_kv_uint("cap", SCAN_RECORDS_CAP);
    tw32_json_kv_bool("truncated", count > got);
    tw32_json_end();

    s_state.aps_seen += got;
    s_state.scans_done++;
}

static void scanner_task(void *arg)
{
    (void)arg;
    /* Inter-sweep wait is sliced into ~50 ms chunks so a CLI `stop`
     * (or a long-then-short `interval` change) takes effect within
     * one slice instead of after up to 600 s. */
    const uint32_t SLICE_MS = 50;
    while (true) {
        if (!s_state.running) {
            vTaskDelay(pdMS_TO_TICKS(SLICE_MS));
            continue;
        }
        scan_once();

        uint32_t remaining = s_state.interval_ms;
        while (remaining > 0 && s_state.running) {
            uint32_t step = remaining > SLICE_MS ? SLICE_MS : remaining;
            vTaskDelay(pdMS_TO_TICKS(step));
            remaining -= step;
        }
    }
}

/* --- CLI command handlers ------------------------------------------------- */

static int cmd_start(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    /* Idempotent: a second `start` while running is a no-op rather
     * than an error so host-side scripts can resync without churn. */
    s_state.running = true;
    tw32_cli_ack_ok("start");
    return 0;
}

static int cmd_stop(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    s_state.running = false;
    /* If the scanner task is currently blocked inside
     * esp_wifi_scan_start(.., true), this nudges the IDF driver to
     * return immediately with ESP_ERR_WIFI_STATE so the user does not
     * have to wait out an in-flight 13-channel sweep (~1.3 s worst
     * case at 100 ms dwell). Safe to call when no scan is active:
     * IDF returns ESP_ERR_WIFI_NOT_STARTED / ESP_FAIL, which we
     * ignore. Reference: esp_wifi.h `esp_wifi_scan_stop`. */
    (void)esp_wifi_scan_stop();
    tw32_cli_ack_ok("stop");
    return 0;
}

static int cmd_interval(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) {
        tw32_cli_ack_err("interval", "missing_arg");
        return -1;
    }
    long ms = strtol(argv[1], NULL, 10);
    if (ms < 100 || ms > 600000) {
        tw32_cli_ack_err("interval", "out_of_range");
        return -1;
    }
    s_state.interval_ms = (uint32_t)ms;
    tw32_cli_ack_ok("interval");
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("scans_done", s_state.scans_done);
    tw32_json_kv_uint("aps_seen",   s_state.aps_seen);
    tw32_json_kv_uint("interval_ms", s_state.interval_ms);
    tw32_json_kv_uint("records_cap", SCAN_RECORDS_CAP);
    tw32_json_kv_bool("running", s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start",    "begin scanning",                      cmd_start    },
    { "stop",     "halt scanning",                       cmd_stop     },
    { "interval", "interval N (ms between sweeps)",       cmd_interval },
    { "stats",    "print counters",                      cmd_stats    },
};

/* --- Wi-Fi bring-up ------------------------------------------------------- */

static void wifi_init_passive(void)
{
    { esp_err_t _e = esp_event_loop_create_default();
      if (_e != ESP_OK && _e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(_e); }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Don't connect — we only scan. */
    esp_wifi_set_ps(WIFI_PS_NONE);
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot");

    tw32_nvs_init();
    tw32_cdc_init();
    wifi_init_passive();

    /* Pin the scanner task to core 1 so the Wi-Fi driver on core 0
     * keeps tight RF timing. */
    xTaskCreatePinnedToCore(scanner_task, "tw32-scan", 6144, NULL, 4, NULL, 1);

    /* Hand control to the CLI dispatcher. Returns only on fatal error. */
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
