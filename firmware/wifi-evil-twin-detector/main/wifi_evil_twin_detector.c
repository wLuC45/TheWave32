/*
 * TheWave32 / wifi-evil-twin-detector
 *
 * Defensive counterpart to wifi-evil-twin / wifi-bssid-clone. Maintains
 * a small whitelist of (SSID, BSSID) pairs in NVS: the "real" APs you
 * trust, and periodically re-scans the air. Whenever a known SSID
 * appears with a BSSID that is NOT on the whitelist, an `event:
 * "rogue_ap"` is emitted.
 *
 * This catches:
 *  - Open clones of your AP that mimic only the SSID (Hydra32 Evil Twin).
 *  - Twin-deauth attacks that clone SSID + BSSID on a different channel
 *    (we report the channel mismatch as a separate `channel_mismatch`
 *    event for the same BSSID).
 *
 * Does NOT catch attacks that hide the SSID (hidden APs are generally
 * ignored). Re-scan cadence is configurable (default 30 s).
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"
#include "tw32_wifi_scan.h"

#define MODULE_NAME    "wifi-evil-twin-detector"
#define MODULE_VERSION "0.2.0"

#define MAX_WHITELIST 16
#define SSID_MAX 32

typedef struct {
    bool     used;
    char     ssid[SSID_MAX + 1];
    uint8_t  bssid[6];
    uint8_t  channel;       /* expected channel (0 = any) */
} known_ap_t;

typedef struct {
    /* `running` and `interval_s` are written by CLI command handlers and read
     * by watcher_task on its own core; promote to _Atomic so the cross-task
     * visibility is part of the type rather than a 32-bit-aligned assumption. */
    _Atomic bool      running;
    _Atomic uint32_t  interval_s;
    /* run_one_check() runs from both the watcher task and cmd_scan (CLI),
     * so these counters have two writers plus the stats reader: atomic to
     * avoid lost increments / a torn read. */
    _Atomic uint32_t  scans_done;
    _Atomic uint32_t  alerts;
    known_ap_t        list[MAX_WHITELIST];
    SemaphoreHandle_t lock;
} state_t;

static state_t s_state = { .interval_s = 30 };

#define LOCK()   xSemaphoreTake(s_state.lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_state.lock)

static int parse_mac(const char *s, uint8_t mac[6])
{
    if (!s || strlen(s) != 17) return -1;
    for (int i = 2; i <= 14; i += 3) if (s[i] != ':') return -1;
    int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) return -1;
    for (int i = 0; i < 6; i++) {
        if (v[i] < 0 || v[i] > 0xFF) return -1;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

/* IEEE 802.11-2020 9.4.2.2 lets the SSID be up to 32 arbitrary octets,
 * including ',' (0x2C) and '|' (0x7C). Our NVS CSV grammar uses those two
 * bytes as separators, so any user-supplied SSID has to be sanitised before
 * it touches the whitelist; otherwise the load_from_nvs splitter slices an
 * entry into pieces and silently drops or rewrites neighbours. We replace
 * the two reserved bytes (and any control char) with '_' on the way in.
 * Same defensive shape as wifi-mac-tracker's label sanitisation. */
static void sanitize_ssid(const char *in, char *out, size_t out_sz)
{
    size_t n = strnlen(in, SSID_MAX);
    if (n >= out_sz) n = out_sz - 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch == ',' || ch == '|' || ch < 0x20) ch = '_';
        out[i] = (char)ch;
    }
    out[n] = '\0';
}

static int find_by_ssid_locked(const char *ssid)
{
    for (int i = 0; i < MAX_WHITELIST; i++) {
        if (s_state.list[i].used && strcmp(s_state.list[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

static int find_free_locked(void)
{
    for (int i = 0; i < MAX_WHITELIST; i++) if (!s_state.list[i].used) return i;
    return -1;
}

/* --- NVS persistence: "ssid|bssid|chan,ssid|bssid|chan,..." ------- */

static void persist_locked(void)
{
    nvs_handle_t h;
    if (nvs_open("etwd", NVS_READWRITE, &h) != ESP_OK) return;
    char buf[(SSID_MAX + 1 + 17 + 1 + 3 + 1) * MAX_WHITELIST + 1] = {0};
    size_t pos = 0;
    for (int i = 0; i < MAX_WHITELIST; i++) {
        if (!s_state.list[i].used) continue;
        if (pos > 0) buf[pos++] = ',';
        int n = snprintf(buf + pos, sizeof(buf) - pos,
                         "%s|%02x:%02x:%02x:%02x:%02x:%02x|%u",
                         s_state.list[i].ssid,
                         s_state.list[i].bssid[0], s_state.list[i].bssid[1],
                         s_state.list[i].bssid[2], s_state.list[i].bssid[3],
                         s_state.list[i].bssid[4], s_state.list[i].bssid[5],
                         (unsigned)s_state.list[i].channel);
        if (n > 0) pos += (size_t)n;
    }
    nvs_set_str(h, "wl", buf);
    nvs_commit(h);
    nvs_close(h);
}

static void load_from_nvs(void)
{
    /* CSV grammar written by persist_locked():
     *   entry := ssid "|" "%02x:%02x:%02x:%02x:%02x:%02x" "|" decimal
     *   blob  := entry ("," entry)*
     *
     * The naive `strtok(buf, ",")` + `strchr(tok, '|')` parse mis-handles a
     * real case: if the SSID contains a literal ',' or '|' (both are valid
     * SSID octets per IEEE 802.11-2020 9.4.2.2) the parser slices the entry
     * in the wrong place and the rest of the blob is corrupted. cmd_add now
     * strips those bytes before they hit the list, but historical NVS data
     * may still contain them, so we parse positionally and resync on errors.
     *
     * Strategy: from the current cursor, walk forward looking for "|XX:XX:..
     * XX|" where XX:..XX is a 17-char MAC. The first such window after the
     * cursor terminates the SSID and gives us the MAC and channel. If no
     * window is found before the next ',' (or end-of-string), the entry is
     * dropped and we resync at the next ','. */
    char buf[(SSID_MAX + 1 + 17 + 1 + 3 + 1) * MAX_WHITELIST + 1] = {0};
    if (!tw32_nvs_get_str("etwd", "wl", buf, sizeof(buf))) return;
    LOCK();
    size_t pos = 0;
    size_t len = strnlen(buf, sizeof(buf));
    while (pos < len) {
        while (pos < len && buf[pos] == ',') pos++;
        if (pos >= len) break;
        /* Find the SSID/MAC boundary: a '|' followed by 17 chars then '|'. */
        size_t entry_end = pos;
        while (entry_end < len && buf[entry_end] != ',') entry_end++;
        size_t bar1 = pos;
        bool found = false;
        /* Need bar1 itself plus 17 MAC chars plus the closing '|' in range. */
        while (bar1 + 18 < entry_end) {
            if (buf[bar1] == '|' && buf[bar1 + 18] == '|') { found = true; break; }
            bar1++;
        }
        if (!found) { pos = entry_end; continue; }
        size_t ssid_len = bar1 - pos;
        if (ssid_len == 0 || ssid_len > SSID_MAX) { pos = entry_end; continue; }
        char mac_str[18];
        memcpy(mac_str, buf + bar1 + 1, 17);
        mac_str[17] = '\0';
        uint8_t mac[6];
        if (parse_mac(mac_str, mac) != 0) { pos = entry_end; continue; }
        size_t chan_start = bar1 + 1 + 17 + 1;
        long ch = (chan_start < entry_end) ? strtol(buf + chan_start, NULL, 10) : 0;
        if (ch < 0 || ch > 255) ch = 0;
        int slot = find_free_locked();
        if (slot >= 0) {
            memset(&s_state.list[slot], 0, sizeof(known_ap_t));
            s_state.list[slot].used = true;
            memcpy(s_state.list[slot].ssid, buf + pos, ssid_len);
            s_state.list[slot].ssid[ssid_len] = '\0';
            memcpy(s_state.list[slot].bssid, mac, 6);
            s_state.list[slot].channel = (uint8_t)ch;
        }
        pos = entry_end;
    }
    UNLOCK();
}

/* --- detection ----------------------------------------------------- */

static void run_one_check(void)
{
    wifi_ap_record_t records[TW32_WIFI_SCAN_MAX];
    int count = tw32_wifi_scan_silent(records, TW32_WIFI_SCAN_MAX);
    /* Count every attempted sweep, including the empty/RF-quiet case, so the
     * `scans_done` counter reflects watcher cadence and is comparable across
     * environments. A negative return is a driver error and is excluded. */
    if (count < 0) return;
    s_state.scans_done++;
    if (count == 0) return;

    /* Take a snapshot of the whitelist to compare without holding the lock
     * while we emit JSON. */
    static known_ap_t snap[MAX_WHITELIST];
    LOCK();
    memcpy(snap, s_state.list, sizeof(snap));
    UNLOCK();

    for (int i = 0; i < count; i++) {
        const wifi_ap_record_t *ap = &records[i];
        for (int j = 0; j < MAX_WHITELIST; j++) {
            if (!snap[j].used) continue;
            if (strcmp(snap[j].ssid, (const char *)ap->ssid) != 0) continue;
            /* Same SSID: compare BSSIDs (IEEE 802.11-2020 9.4.1.1 - BSSID is
             * the unique identifier of the AP's BSS, so a same-SSID + new
             * BSSID is the classic evil-twin signature, cf. Bauer et al.,
             * "Physical-Layer Identification of Wireless Devices"). */
            if (memcmp(snap[j].bssid, ap->bssid, 6) != 0) {
                s_state.alerts++;
                tw32_json_begin();
                tw32_json_kv_str("event",        "rogue_ap");
                tw32_json_kv_str("ssid",         (const char *)ap->ssid);
                tw32_json_kv_mac("expected_bssid", snap[j].bssid);
                tw32_json_kv_mac("actual_bssid",   ap->bssid);
                tw32_json_kv_int("channel",      ap->primary);
                tw32_json_kv_int("rssi",         ap->rssi);
                tw32_json_end();
            } else if (snap[j].channel && ap->primary != snap[j].channel) {
                /* Same BSSID but the AP appears on a different primary channel
                 * than expected: this is the twin-deauth signature (clone the
                 * MAC, ride a different channel after kicking clients off).
                 * Counted as an alert so `stats.alerts` matches user intuition. */
                s_state.alerts++;
                tw32_json_begin();
                tw32_json_kv_str("event",       "channel_mismatch");
                tw32_json_kv_str("ssid",        (const char *)ap->ssid);
                tw32_json_kv_mac("bssid",       ap->bssid);
                tw32_json_kv_int("expected_ch", snap[j].channel);
                tw32_json_kv_int("actual_ch",   ap->primary);
                tw32_json_end();
            }
        }
    }
}

static void watcher_task(void *arg)
{
    (void)arg;
    while (true) {
        if (!s_state.running) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        run_one_check();
        for (uint32_t s = 0; s < s_state.interval_s && s_state.running; s++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

/* --- bring-up ------------------------------------------------------ */

static void wifi_init(void)
{
    { esp_err_t _e = esp_event_loop_create_default();
      if (_e != ESP_OK && _e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(_e); }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
}

/* --- CLI ----------------------------------------------------------- */

static int cmd_add(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 3) { tw32_cli_ack_err("add", "usage: add <ssid> <bssid> [chan]"); return -1; }
    size_t L = strlen(argv[1]);
    if (L == 0 || L > SSID_MAX) { tw32_cli_ack_err("add", "bad_ssid_len"); return -1; }
    uint8_t mac[6];
    if (parse_mac(argv[2], mac) != 0) { tw32_cli_ack_err("add", "bad_bssid"); return -1; }
    int chan = 0;
    if (argc >= 4) {
        long ch = strtol(argv[3], NULL, 10);
        if (ch < 1 || ch > 14) { tw32_cli_ack_err("add", "bad_channel"); return -1; }
        chan = (int)ch;
    }
    /* Sanitise before locking so we compare-by-stored-form in find_by_ssid_locked. */
    char ssid_clean[SSID_MAX + 1];
    sanitize_ssid(argv[1], ssid_clean, sizeof(ssid_clean));
    LOCK();
    int existing = find_by_ssid_locked(ssid_clean);
    int slot = (existing >= 0) ? existing : find_free_locked();
    if (slot < 0) {
        UNLOCK();
        tw32_cli_ack_err("add", "list_full");
        return -1;
    }
    memset(&s_state.list[slot], 0, sizeof(known_ap_t));
    s_state.list[slot].used = true;
    memcpy(s_state.list[slot].ssid, ssid_clean, strlen(ssid_clean));
    memcpy(s_state.list[slot].bssid, mac, 6);
    s_state.list[slot].channel = (uint8_t)chan;
    persist_locked();
    UNLOCK();
    tw32_cli_ack_ok("add");
    return 0;
}

static int cmd_remove(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("remove", "missing_arg"); return -1; }
    LOCK();
    int idx = find_by_ssid_locked(argv[1]);
    if (idx < 0) {
        UNLOCK();
        tw32_cli_ack_err("remove", "not_in_list");
        return -1;
    }
    s_state.list[idx].used = false;
    persist_locked();
    UNLOCK();
    tw32_cli_ack_ok("remove");
    return 0;
}

static int cmd_list(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    int count = 0;
    static known_ap_t snap[MAX_WHITELIST];
    LOCK();
    memcpy(snap, s_state.list, sizeof(snap));
    UNLOCK();
    for (int i = 0; i < MAX_WHITELIST; i++) {
        if (!snap[i].used) continue;
        count++;
        tw32_json_begin();
        tw32_json_kv_str("event", "known");
        tw32_json_kv_int("idx", i);
        tw32_json_kv_str("ssid", snap[i].ssid);
        tw32_json_kv_mac("bssid", snap[i].bssid);
        tw32_json_kv_int("channel", snap[i].channel);
        tw32_json_end();
    }
    tw32_json_begin();
    tw32_json_kv_str("cmd", "list");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_int("count", count);
    tw32_json_end();
    return 0;
}

static int cmd_clear(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    LOCK();
    memset(s_state.list, 0, sizeof(s_state.list));
    persist_locked();
    UNLOCK();
    tw32_cli_ack_ok("clear");
    return 0;
}

static int cmd_interval(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("interval", "missing_arg"); return -1; }
    long s = strtol(argv[1], NULL, 10);
    if (s < 5 || s > 3600) { tw32_cli_ack_err("interval", "out_of_range"); return -1; }
    s_state.interval_s = (uint32_t)s;
    tw32_cli_ack_ok("interval");
    return 0;
}

static int cmd_scan(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    /* One-shot: emits all rogue/mismatch events from a fresh scan, plus a
     * cmd:scan terminator. */
    run_one_check();
    tw32_json_begin();
    tw32_json_kv_str("cmd", "scan");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("scans_done", s_state.scans_done);
    tw32_json_kv_uint("alerts", s_state.alerts);
    tw32_json_end();
    return 0;
}

static int cmd_start(tw32_cli_ctx_t *c, int argc, char **argv)
{ (void)c;(void)argc;(void)argv; s_state.running = true;  tw32_cli_ack_ok("start"); return 0; }

static int cmd_stop(tw32_cli_ctx_t *c, int argc, char **argv)
{ (void)c;(void)argc;(void)argv; s_state.running = false; tw32_cli_ack_ok("stop");  return 0; }

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    int watched = 0;
    LOCK();
    for (int i = 0; i < MAX_WHITELIST; i++) if (s_state.list[i].used) watched++;
    UNLOCK();
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("scans_done", s_state.scans_done);
    tw32_json_kv_uint("alerts",     s_state.alerts);
    tw32_json_kv_int ("whitelist",  watched);
    tw32_json_kv_uint("interval_s", s_state.interval_s);
    tw32_json_kv_bool("running",    s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "add",      "add <ssid> <bssid> [chan]",   cmd_add      },
    { "remove",   "remove <ssid>",               cmd_remove   },
    { "list",     "dump whitelist",              cmd_list     },
    { "clear",    "purge whitelist",             cmd_clear    },
    { "interval", "interval seconds (5..3600)",  cmd_interval },
    { "scan",     "one-shot check now",          cmd_scan     },
    { "start",    "begin periodic watching",     cmd_start    },
    { "stop",     "halt watching",               cmd_stop     },
    { "stats",    "counters + state",            cmd_stats    },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();
    s_state.lock = xSemaphoreCreateMutex();
    configASSERT(s_state.lock);
    load_from_nvs();
    wifi_init();
    tw32_wifi_scan_init();
    xTaskCreatePinnedToCore(watcher_task, "tw32-etwd", 4096, NULL, 4, NULL, 1);
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
