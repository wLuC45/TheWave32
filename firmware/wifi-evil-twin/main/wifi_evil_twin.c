/*
 * TheWave32 / wifi-evil-twin (skeleton — phase 1)
 *
 * Brings up an open SoftAP with a configurable SSID/channel and logs
 * every client connect/disconnect via the ESP-IDF Wi-Fi event API. This
 * is the phase-1 scaffold for the full Hydra32 Evil Twin attack; phase
 * 2 will add the captive portal HTTP server, DNS hijack, and password
 * harvest endpoint, plus optional concurrent deauth against the
 * legitimate target.
 *
 * Phase 2 TODO (deferred):
 *   - esp_netif_create_default_wifi_ap + DHCP (today: SoftAP without
 *     IP stack — clients negotiate L2 association only).
 *   - HTTP server on port 80 (esp_http_server) serving a fake captive
 *     portal index page.
 *   - DNS responder bound to 0.0.0.0:53 returning the AP IP for any
 *     query (captive portal trigger).
 *   - POST /submit handler that records the password attempt and
 *     attempts STA association against the real AP to verify.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "tw32_ap_security.h"
#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"
#include "tw32_wifi_scan.h"

#define MODULE_NAME    "wifi-evil-twin"
#define MODULE_VERSION "0.1.0"

#define MAX_CLIENTS 8
#define MAX_SSID_LEN 32

typedef struct {
    char     ssid[MAX_SSID_LEN + 1];
    uint8_t  ssid_len;
    uint8_t  channel;
    bool     have_target;
    tw32_ap_security_t sec;     /* shared password / auth / hidden */
    /* Cross-context flags / counters. The Wi-Fi event task increments
     * the totals; CLI task reads them from cmd_stats. Per project
     * concurrency doctrine (see wifi-clock-skew sibling) these are
     * C11 atomics so cmd_stats can snapshot lock-free. */
    atomic_bool       running;
    _Atomic uint32_t  total_connects;
    _Atomic uint32_t  total_disconnects;
    /* Live connected MACs. Mutated by the Wi-Fi event task on
     * AP_STA{CONNECTED,DISCONNECTED} and zeroed by cmd_stop; always
     * touched under `lock`. */
    uint8_t  clients[MAX_CLIENTS][6];
    uint8_t  clients_used;
    SemaphoreHandle_t lock;
} state_t;

static state_t s_state = { .channel = 1 };

#define LOCK()   xSemaphoreTake(s_state.lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_state.lock)

static void emit_client_event(const char *evt, const uint8_t *mac, int reason)
{
    tw32_json_begin();
    tw32_json_kv_str("event", evt);
    tw32_json_kv_mac("mac", mac);
    if (reason >= 0) tw32_json_kv_int("reason", reason);
    tw32_json_end();
}

static void wifi_event_cb(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        atomic_fetch_add_explicit(&s_state.total_connects, 1,
                                  memory_order_relaxed);
        LOCK();
        if (s_state.clients_used < MAX_CLIENTS) {
            memcpy(s_state.clients[s_state.clients_used++], e->mac, 6);
        }
        UNLOCK();
        emit_client_event("client_connected", e->mac, -1);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        atomic_fetch_add_explicit(&s_state.total_disconnects, 1,
                                  memory_order_relaxed);
        LOCK();
        for (int i = 0; i < s_state.clients_used; i++) {
            if (memcmp(s_state.clients[i], e->mac, 6) == 0) {
                /* Only shift when there is a tail; clients[i+1] with
                 * i == clients_used-1 would form an out-of-bounds
                 * pointer (UB even though the copy length is 0). */
                int tail = s_state.clients_used - i - 1;
                if (tail > 0) {
                    memmove(s_state.clients[i], s_state.clients[i + 1],
                            (size_t)tail * 6);
                }
                s_state.clients_used--;
                break;
            }
        }
        UNLOCK();
        emit_client_event("client_disconnected", e->mac, e->reason);
    }
}

static void wifi_init(void)
{
    { esp_err_t _e = esp_event_loop_create_default();
      if (_e != ESP_OK && _e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(_e); }
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_cb, NULL));
    /* APSTA mode lets us scan via STA and beacon as AP from the same
     * boot. The AP starts with a stub SSID so beacons stay inert until
     * `start` swaps in the real one. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t stub = {0};
    /* 1-char placeholder SSID — AP refuses to start with ssid_len == 0. */
    stub.ap.ssid[0]    = '_';
    stub.ap.ssid_len   = 1;
    stub.ap.channel    = 1;
    stub.ap.authmode   = WIFI_AUTH_OPEN;
    stub.ap.max_connection = MAX_CLIENTS;
    stub.ap.beacon_interval = 100;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &stub));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
}

static esp_err_t apply_ap_config(void)
{
    if (!s_state.have_target) return ESP_ERR_INVALID_STATE;
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, s_state.ssid, s_state.ssid_len);
    ap.ap.ssid_len        = s_state.ssid_len;
    ap.ap.channel         = s_state.channel;
    ap.ap.max_connection  = MAX_CLIENTS;
    ap.ap.beacon_interval = 100;
    esp_err_t e = tw32_ap_security_apply(&s_state.sec, &ap);
    if (e != ESP_OK) return e;
    return esp_wifi_set_config(WIFI_IF_AP, &ap);
}

/* --- CLI ---------------------------------------------------------------- */

static int cmd_scan(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    if (tw32_wifi_scan_run() < 0) {
        tw32_cli_ack_err("scan", "scan_failed");
        return -1;
    }
    return 0;
}

static int cmd_target(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) {
        tw32_cli_ack_err("target", "usage: target <N> | target <ssid> <chan>");
        return -1;
    }
    /* `target <N>` — pull SSID + channel from scan cache. */
    if (argc == 2) {
        tw32_ap_entry_t entry;
        if (!tw32_wifi_scan_resolve_index(argv[1], &entry)) {
            tw32_cli_ack_err("target", "bad_index"); return -1;
        }
        size_t L = strlen(entry.ssid);
        if (L == 0 || L > MAX_SSID_LEN) {
            tw32_cli_ack_err("target", "bad_ssid_len"); return -1;
        }
        memcpy(s_state.ssid, entry.ssid, L);
        s_state.ssid[L] = '\0';
        s_state.ssid_len = (uint8_t)L;
        s_state.channel = entry.channel;
        s_state.have_target = true;
        tw32_json_begin();
        tw32_json_kv_str("cmd", "target");
        tw32_json_kv_bool("ok", true);
        tw32_json_kv_str("ssid", s_state.ssid);
        tw32_json_kv_int("channel", s_state.channel);
        tw32_json_end();
        return 0;
    }
    /* `target <ssid> <chan>` — explicit. */
    size_t L = strlen(argv[1]);
    if (L == 0 || L > MAX_SSID_LEN) { tw32_cli_ack_err("target", "bad_ssid_len"); return -1; }
    long ch = strtol(argv[2], NULL, 10);
    if (ch < 1 || ch > 14) { tw32_cli_ack_err("target", "bad_channel"); return -1; }
    memcpy(s_state.ssid, argv[1], L);
    s_state.ssid[L] = '\0';
    s_state.ssid_len = (uint8_t)L;
    s_state.channel = (uint8_t)ch;
    s_state.have_target = true;
    tw32_cli_ack_ok("target");
    return 0;
}

static int cmd_start(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    if (!s_state.have_target) { tw32_cli_ack_err("start", "no_target"); return -1; }
    /* Re-applying ap config while running kicks every associated STA
     * (esp_wifi_set_config on the AP interface re-broadcasts the new
     * config and forces re-association). Gate the no-op case. */
    if (atomic_load_explicit(&s_state.running, memory_order_acquire)) {
        tw32_cli_ack_err("start", "already_running"); return -1;
    }
    if (apply_ap_config() != ESP_OK) {
        tw32_cli_ack_err("start", "config_failed"); return -1;
    }
    atomic_store_explicit(&s_state.running, true, memory_order_release);
    tw32_cli_ack_ok("start");
    return 0;
}

static int cmd_stop(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    /* Swap AP config back to the inert stub so beacons stop matching
     * the target SSID, while keeping the radio + netif up so future
     * scans / phase-2 captive portal work without a full restart.
     * Per ESP-IDF SoftAP docs the config swap forces associated STAs
     * to deauthenticate; the resulting AP_STADISCONNECTED events will
     * race us, so we flip `running` BEFORE the swap and only drain
     * `clients_used` AFTER the swap to absorb the stragglers. */
    atomic_store_explicit(&s_state.running, false, memory_order_release);
    wifi_config_t stub = {0};
    stub.ap.ssid[0]    = '_';
    stub.ap.ssid_len   = 1;
    stub.ap.channel    = 1;
    stub.ap.authmode   = WIFI_AUTH_OPEN;
    stub.ap.max_connection = MAX_CLIENTS;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_AP, &stub);
    LOCK();
    s_state.clients_used = 0;
    UNLOCK();
    if (e != ESP_OK) {
        /* Beaconing the rogue SSID may still be active; surface so the
         * operator does not falsely believe the AP is down. */
        tw32_cli_ack_err("stop", "stub_swap_failed");
        return -1;
    }
    tw32_cli_ack_ok("stop");
    return 0;
}

static int cmd_clients(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    /* Snapshot to emit outside the lock. Stack-local (48 B) rather
     * than `static` so two CLI tasks could theoretically dump without
     * stomping each other. */
    uint8_t snap[MAX_CLIENTS][6];
    LOCK();
    int n = s_state.clients_used;
    memcpy(snap, s_state.clients, sizeof(snap));
    UNLOCK();
    for (int i = 0; i < n; i++) {
        tw32_json_begin();
        tw32_json_kv_str("event", "client");
        tw32_json_kv_mac("mac", snap[i]);
        tw32_json_end();
    }
    tw32_json_begin();
    tw32_json_kv_str("cmd", "clients");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_int("count", n);
    tw32_json_end();
    return 0;
}

/* Three thin wrappers over tw32_ap_security_*. The shared helper does
 * all the validation; we only marshal the JSON ack. */
static int cmd_password(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    char err[TW32_AP_ERROR_SIZE] = {0};
    const char *arg = argc >= 2 ? argv[1] : NULL;
    if (tw32_ap_security_set_password(&s_state.sec, arg, err) == 0)
        tw32_cli_ack_ok("password");
    else
        tw32_cli_ack_err("password", err);
    return 0;
}

static int cmd_auth(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    char err[TW32_AP_ERROR_SIZE] = {0};
    const char *arg = argc >= 2 ? argv[1] : NULL;
    if (tw32_ap_security_set_auth(&s_state.sec, arg, err) == 0)
        tw32_cli_ack_ok("auth");
    else
        tw32_cli_ack_err("auth", err);
    return 0;
}

static int cmd_hidden(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    char err[TW32_AP_ERROR_SIZE] = {0};
    const char *arg = argc >= 2 ? argv[1] : NULL;
    if (tw32_ap_security_set_hidden(&s_state.sec, arg, err) == 0)
        tw32_cli_ack_ok("hidden");
    else
        tw32_cli_ack_err("hidden", err);
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    /* Counters are atomics so load lock-free. `clients_used` is
     * mutex-protected (event task can be mid-memmove) so snapshot it
     * under LOCK. */
    uint32_t connects    = atomic_load_explicit(&s_state.total_connects,
                                                memory_order_relaxed);
    uint32_t disconnects = atomic_load_explicit(&s_state.total_disconnects,
                                                memory_order_relaxed);
    bool     running     = atomic_load_explicit(&s_state.running,
                                                memory_order_acquire);
    LOCK();
    int      connected   = s_state.clients_used;
    UNLOCK();
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("total_connects",    connects);
    tw32_json_kv_uint("total_disconnects", disconnects);
    tw32_json_kv_int ("connected_count",   connected);
    if (s_state.have_target) {
        tw32_json_kv_str("target_ssid", s_state.ssid);
        tw32_json_kv_int("channel", s_state.channel);
    }
    tw32_ap_security_emit_stats(&s_state.sec);
    tw32_json_kv_bool("running",      running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "password","password <pwd|clear> (8..63 ASCII; sets WPA2)", cmd_password },
    { "auth",    "auth <open|wpa2>",                           cmd_auth    },
    { "hidden",  "hidden <on|off> (cloak SSID in beacons)",    cmd_hidden  },
    { "scan",    "list nearby APs",                            cmd_scan    },
    { "target",  "target <N> | target <ssid> <chan>",          cmd_target  },
    { "start",   "bring up rogue AP",                          cmd_start   },
    { "stop",    "halt rogue AP",                              cmd_stop    },
    { "clients", "dump client list",                           cmd_clients },
    { "stats",   "AP + client stats",                          cmd_stats   },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();
    s_state.lock = xSemaphoreCreateMutex();
    configASSERT(s_state.lock);
    wifi_init();
    tw32_wifi_scan_init();
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
