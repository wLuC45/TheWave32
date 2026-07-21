/*
 * TheWave32 / wifi-handshake-capture
 *
 * Capture WPA/WPA2 4-way handshakes in promiscuous mode and export the
 * crackable material as a Hashcat 22000 line (mode WPA*02*).
 *
 * The promiscuous callback copies the raw 802.1X EAPOL-Key frame of
 * every M1..M4 it sees. The drain task caches M1 (ANonce + replay
 * counter) and, when the matching M2 arrives for the same (BSSID,STA)
 * with the same replay counter, assembles and emits the 22000 line:
 *
 *   WPA*02*MIC*MAC_AP*MAC_STA*ESSID*ANONCE*EAPOL*MESSAGEPAIR
 *
 * MIC and EAPOL come from M2 (with the MIC field zeroed inside EAPOL),
 * ANONCE from M1. The ESSID is not in the EAPOL frames, so the line is
 * only emitted when a target was picked from the scan cache (`target N`
 * after `scan`), which is where the SSID is known.
 *
 * The Key Information field (IEEE 802.11i):
 *   bit 0..2 = Key Descriptor Version
 *   bit 3    = Key Type (1 = pairwise)
 *   bit 6    = Install
 *   bit 7    = Key Ack
 *   bit 8    = Key MIC
 *   bit 9    = Secure
 *
 * 4-way handshake message classification (pairwise = 1):
 *   M1: Ack=1, Mic=0, Install=0, Secure=0
 *   M2: Ack=0, Mic=1, Install=0, Secure=0
 *   M3: Ack=1, Mic=1, Install=1, Secure=1
 *   M4: Ack=0, Mic=1, Install=0, Secure=1
 *
 * EAPOL-Key frame layout, offsets from the start of the 802.1X frame:
 *   0      EAPOL version
 *   1      EAPOL type (0x03 = Key)
 *   2..3   EAPOL body length (big-endian)
 *   4      descriptor type
 *   5..6   key info (big-endian)
 *   7..8   key length
 *   9..16  replay counter
 *   17..48 key nonce  (ANonce in M1/M3, SNonce in M2/M4)
 *   49..64 key IV
 *   65..72 key RSC
 *   73..80 reserved
 *   81..96 key MIC
 *   97..98 key data length
 *   99..   key data
 */

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
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"
#include "tw32_wifi_scan.h"

#define MODULE_NAME    "wifi-handshake-capture"
#define MODULE_VERSION "0.2.1"

#define LLC_SNAP_LEN    8
#define ETHERTYPE_EAPOL 0x888E
#define EAPOL_TYPE_KEY  0x03

#define KI_PAIRWISE  (1u << 3)
#define KI_INSTALL   (1u << 6)
#define KI_ACK       (1u << 7)
#define KI_MIC       (1u << 8)
#define KI_SECURE    (1u << 9)

/* Offsets inside the 802.1X frame. */
#define L2X_REPLAY_OFF  9
#define L2X_NONCE_OFF   17
#define L2X_MIC_OFF     81
#define L2X_MIN_LEN     99   /* 4-byte EAPOL header + 95-byte descriptor */

/* Max 802.1X frame captured. A real M2 is ~121 bytes; the cap is a
 * safety bound and the ceiling Hashcat itself uses for the EAPOL field. */
#define EAPOL_CAP       256

typedef struct {
    volatile bool     running;
    volatile uint8_t  channel;
    volatile bool     have_target;
    uint8_t           target_bssid[6];
    bool              have_ssid;
    char              target_ssid[TW32_WIFI_SSID_MAX + 1];
    /* Counters: eapol_seen/dropped are written by the Wi-Fi RX callback,
     * the rest by the drain task, all read by the CLI on another core.
     * Atomic so the cross-core RMW/read is defined rather than a race. */
    _Atomic uint32_t eapol_seen;
    _Atomic uint32_t m1_seen;
    _Atomic uint32_t m2_seen;
    _Atomic uint32_t m3_seen;
    _Atomic uint32_t m4_seen;
    _Atomic uint32_t handshakes_complete;
    _Atomic uint32_t hashes_emitted;   /* 22000 lines emitted */
    _Atomic uint32_t dropped;          /* events lost to a full queue */
} state_t;

static state_t s_state = { .channel = 1 };

/* Record handed from the Wi-Fi RX callback to the drain task. Carries
 * the raw 802.1X frame so the drain task can assemble the 22000 line
 * off the RX path. */
typedef struct {
    uint32_t ts_us;
    uint8_t  bssid[6];
    uint8_t  sta[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  msg;                 /* 1..4 = M1..M4 */
    uint16_t eapol_len;           /* captured 802.1X frame length */
    uint8_t  eapol[EAPOL_CAP];    /* the 802.1X frame */
} eapol_evt_t;

/* EAPOL events are rare; a queue of 8 (~2.3 KB) absorbs a full handshake. */
#define HS_Q_LEN 8
static QueueHandle_t s_hs_q;

static int classify_msg(uint16_t ki)
{
    if (!(ki & KI_PAIRWISE)) return 0;
    bool ack    = ki & KI_ACK;
    bool mic    = ki & KI_MIC;
    bool inst   = ki & KI_INSTALL;
    bool secure = ki & KI_SECURE;
    if ( ack && !mic && !inst && !secure) return 1;  /* M1 */
    if (!ack &&  mic && !inst && !secure) return 2;  /* M2 */
    if ( ack &&  mic &&  inst &&  secure) return 3;  /* M3 */
    if (!ack &&  mic && !inst &&  secure) return 4;  /* M4 */
    return 0;
}

static int hex_n(char *out, const uint8_t *in, int n)
{
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[2 * i]     = H[in[i] >> 4];
        out[2 * i + 1] = H[in[i] & 0x0f];
    }
    return n * 2;
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_state.running) return;
    if (type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *fr = p->payload;
    uint16_t flen = p->rx_ctrl.sig_len;

    if (flen < 24 + LLC_SNAP_LEN + L2X_MIN_LEN) return;
    uint8_t fc0 = fr[0], fc1 = fr[1];
    /* Data type subtypes 0x08 (data) and 0x88 (qos-data). */
    if (fc0 != 0x08 && fc0 != 0x88) return;
    if (fc1 & 0x40) return;  /* protected, body unreadable */

    /* IEEE 802.11 address fields depend on the ToDS / FromDS bits:
     *   ToDS=0 FromDS=0 (IBSS):    A1=DA,    A2=SA,    A3=BSSID
     *   ToDS=0 FromDS=1 (AP->STA): A1=DA,    A2=BSSID, A3=SA
     *   ToDS=1 FromDS=0 (STA->AP): A1=BSSID, A2=SA,    A3=DA
     *   ToDS=1 FromDS=1 (WDS):     no plain BSSID (A4 carries SA)
     * The 4-way handshake is exchanged between an AP and a STA, so WDS
     * is out of scope and we skip it. Compute BSSID/STA offsets from
     * the DS bits, not from a content heuristic that mis-identifies
     * AP->STA frames (M1/M3) and swaps BSSID/STA on STA->AP frames. */
    bool to_ds   = fc1 & 0x01;
    bool from_ds = fc1 & 0x02;
    if (to_ds && from_ds) return;  /* WDS, no handshake here */
    uint16_t bssid_off, sta_off;
    if (!to_ds && !from_ds)      { bssid_off = 16; sta_off = 10; } /* IBSS */
    else if (!to_ds && from_ds)  { bssid_off = 10; sta_off = 4;  } /* AP->STA */
    else                         { bssid_off = 4;  sta_off = 10; } /* STA->AP */

    uint16_t hdrlen = (fc0 == 0x88) ? 26 : 24;
    if (flen < hdrlen + LLC_SNAP_LEN + L2X_MIN_LEN) return;

    /* Target filter: now compares against the correct BSSID offset, so
     * AP-originated M1/M3 frames are no longer silently dropped. */
    if (s_state.have_target) {
        if (memcmp(fr + bssid_off, s_state.target_bssid, 6) != 0) return;
    }

    const uint8_t *llc = fr + hdrlen;
    if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03) return;
    uint16_t et = ((uint16_t)llc[6] << 8) | llc[7];
    if (et != ETHERTYPE_EAPOL) return;

    const uint8_t *eapol = llc + LLC_SNAP_LEN;
    if (eapol[1] != EAPOL_TYPE_KEY) return;

    s_state.eapol_seen++;

    const uint8_t *kd = eapol + 4;
    uint16_t ki = ((uint16_t)kd[1] << 8) | kd[2];
    int msg = classify_msg(ki);
    if (msg == 0) return;

    const uint8_t *bssid_in_frame = fr + bssid_off;
    const uint8_t *sta_in_frame   = fr + sta_off;

    /* Full 802.1X frame length = 4-byte header + declared body length,
     * clamped to what is actually in the captured frame and to the cap. */
    uint16_t body = ((uint16_t)eapol[2] << 8) | eapol[3];
    uint32_t l2x  = 4u + body;
    uint32_t avail = flen - (uint32_t)(eapol - fr);
    if (l2x > avail)      l2x = avail;
    if (l2x > EAPOL_CAP)  l2x = EAPOL_CAP;

    eapol_evt_t ev;
    ev.ts_us   = (uint32_t)esp_timer_get_time();
    memcpy(ev.bssid, bssid_in_frame, 6);
    memcpy(ev.sta,   sta_in_frame,   6);
    ev.rssi    = p->rx_ctrl.rssi;
    ev.channel = p->rx_ctrl.channel;
    ev.msg     = (uint8_t)msg;
    ev.eapol_len = (uint16_t)l2x;
    memcpy(ev.eapol, eapol, l2x);
    if (xQueueSend(s_hs_q, &ev, 0) != pdTRUE) s_state.dropped++;
}

/* Assemble and emit the Hashcat 22000 line as a JSON event. The host
 * collects `hash_22000` events and writes them to a .22000 file. */
static void emit_22000(const uint8_t *bssid, const uint8_t *sta,
                       const uint8_t *anonce,
                       const uint8_t *m2_eapol, uint16_t m2_len)
{
    /* EAPOL field: the M2 802.1X frame with its 16-byte MIC zeroed. */
    uint8_t e2[EAPOL_CAP];
    memcpy(e2, m2_eapol, m2_len);
    memset(e2 + L2X_MIC_OFF, 0, 16);

    char line[768];
    int n = 0;
    n += snprintf(line + n, sizeof(line) - n, "WPA*02*");
    n += hex_n(line + n, m2_eapol + L2X_MIC_OFF, 16);   /* MIC from M2 */
    line[n++] = '*';
    n += hex_n(line + n, bssid, 6);
    line[n++] = '*';
    n += hex_n(line + n, sta, 6);
    line[n++] = '*';
    n += hex_n(line + n, (const uint8_t *)s_state.target_ssid,
               (int)strlen(s_state.target_ssid));
    line[n++] = '*';
    n += hex_n(line + n, anonce, 32);
    line[n++] = '*';
    n += hex_n(line + n, e2, m2_len);
    /* MESSAGEPAIR 00 = M1+M2, EAPOL from M2, replay counters matched. */
    n += snprintf(line + n, sizeof(line) - n, "*00");
    line[n] = '\0';

    s_state.hashes_emitted++;
    tw32_json_begin();
    tw32_json_kv_str ("event", "hash_22000");
    tw32_json_kv_mac ("bssid", bssid);
    tw32_json_kv_mac ("sta",   sta);
    tw32_json_kv_str ("line",  line);
    tw32_json_end();
}

/* Drain task: per-pair completion tracking, M1/M2 pairing and 22000
 * export, JSON emission. Single-owner state, no lock. */
static void hs_drain_task(void *arg)
{
    (void)arg;
    eapol_evt_t ev;
    /* M1 cache, paired with the next matching M2. */
    bool     have_m1 = false;
    uint8_t  m1_bssid[6]  = {0};
    uint8_t  m1_sta[6]    = {0};
    uint8_t  m1_anonce[32];
    uint8_t  m1_replay[8];
    uint32_t m1_ts = 0;
    /* M1..M4 completion bitmap, for the handshake_complete event. */
    uint8_t  last_bssid[6]  = {0};
    uint8_t  last_sta[6]    = {0};
    uint8_t  msg_bitmap     = 0;
    uint32_t pair_started_us = 0;

    for (;;) {
        if (xQueueReceive(s_hs_q, &ev, portMAX_DELAY) != pdTRUE) continue;

        switch (ev.msg) {
            case 1: s_state.m1_seen++; break;
            case 2: s_state.m2_seen++; break;
            case 3: s_state.m3_seen++; break;
            case 4: s_state.m4_seen++; break;
        }

        bool same_pair = memcmp(last_bssid, ev.bssid, 6) == 0 &&
                         memcmp(last_sta,   ev.sta,   6) == 0;
        if (!same_pair || (ev.ts_us - pair_started_us) > 5000000u) {
            memcpy(last_bssid, ev.bssid, 6);
            memcpy(last_sta,   ev.sta,   6);
            msg_bitmap = 0;
            pair_started_us = ev.ts_us;
        }
        msg_bitmap |= (1u << (ev.msg - 1));

        tw32_json_begin();
        tw32_json_kv_str ("event", "eapol_msg");
        tw32_json_kv_uint("ts",    ev.ts_us);
        tw32_json_kv_int ("msg",   ev.msg);
        tw32_json_kv_mac ("bssid", ev.bssid);
        tw32_json_kv_mac ("sta",   ev.sta);
        tw32_json_kv_int ("rssi",  ev.rssi);
        tw32_json_kv_int ("ch",    ev.channel);
        tw32_json_end();

        /* --- 22000 pairing --------------------------------------------
         * Cache M1's ANonce + replay counter; when the matching M2
         * arrives (same pair, same replay counter, within 2 s), emit. */
        if (ev.msg == 1 && ev.eapol_len >= L2X_NONCE_OFF + 32) {
            memcpy(m1_bssid,  ev.bssid, 6);
            memcpy(m1_sta,    ev.sta,   6);
            memcpy(m1_anonce, ev.eapol + L2X_NONCE_OFF, 32);
            memcpy(m1_replay, ev.eapol + L2X_REPLAY_OFF, 8);
            m1_ts   = ev.ts_us;
            have_m1 = true;
        } else if (ev.msg == 2 && have_m1
                   && ev.eapol_len >= L2X_MIC_OFF + 16
                   && memcmp(m1_bssid, ev.bssid, 6) == 0
                   && memcmp(m1_sta,   ev.sta,   6) == 0
                   && memcmp(m1_replay, ev.eapol + L2X_REPLAY_OFF, 8) == 0
                   && (ev.ts_us - m1_ts) < 2000000u) {
            if (s_state.have_ssid) {
                emit_22000(ev.bssid, ev.sta, m1_anonce,
                           ev.eapol, ev.eapol_len);
            }
            have_m1 = false;
        }

        if (msg_bitmap == 0x0F) {
            s_state.handshakes_complete++;
            msg_bitmap = 0;
            tw32_json_begin();
            tw32_json_kv_str ("event", "handshake_complete");
            tw32_json_kv_uint("ts",    ev.ts_us);
            tw32_json_kv_mac ("bssid", ev.bssid);
            tw32_json_kv_mac ("sta",   ev.sta);
            tw32_json_end();
        }
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
     * esp_wifi_scan_start (used by the CLI `scan` command). */
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
    /* Insist on the canonical aa:bb:cc:dd:ee:ff layout. */
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

static int cmd_target(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("target", "missing_arg"); return -1; }
    if (!strcmp(argv[1], "any")) {
        s_state.have_target = false;
        s_state.have_ssid   = false;   /* no ESSID, so no 22000 export */
        tw32_cli_ack_ok("target");
        return 0;
    }
    /* 1-based scan-cache index first (e.g. `target 3` after `scan`).
     * This path is the one that yields the ESSID needed for 22000. */
    tw32_ap_entry_t entry;
    if (tw32_wifi_scan_resolve_index(argv[1], &entry)) {
        memcpy(s_state.target_bssid, entry.bssid, 6);
        s_state.have_target = true;
        strncpy(s_state.target_ssid, entry.ssid, TW32_WIFI_SSID_MAX);
        s_state.target_ssid[TW32_WIFI_SSID_MAX] = '\0';
        s_state.have_ssid = (s_state.target_ssid[0] != '\0');
        if (esp_wifi_set_channel(entry.channel, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
            s_state.channel = entry.channel;
        }
        tw32_json_begin();
        tw32_json_kv_str ("cmd", "target");
        tw32_json_kv_bool("ok", true);
        tw32_json_kv_str ("ssid", entry.ssid);
        tw32_json_kv_mac ("bssid", entry.bssid);
        tw32_json_kv_int ("channel", entry.channel);
        tw32_json_end();
        return 0;
    }
    uint8_t m[6];
    if (parse_mac(argv[1], m) != 0) {
        tw32_cli_ack_err("target", "bad_mac_or_index"); return -1;
    }
    memcpy(s_state.target_bssid, m, 6);
    s_state.have_target = true;
    /* A bare MAC carries no ESSID; 22000 export needs `target N`. */
    s_state.have_ssid = false;
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

static int cmd_chan(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("chan", "missing_arg"); return -1; }
    long ch = strtol(argv[1], NULL, 10);
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

static int cmd_stats(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("eapol_seen",          s_state.eapol_seen);
    tw32_json_kv_uint("m1_seen",             s_state.m1_seen);
    tw32_json_kv_uint("m2_seen",             s_state.m2_seen);
    tw32_json_kv_uint("m3_seen",             s_state.m3_seen);
    tw32_json_kv_uint("m4_seen",             s_state.m4_seen);
    tw32_json_kv_uint("handshakes_complete", s_state.handshakes_complete);
    tw32_json_kv_uint("hashes_emitted",      s_state.hashes_emitted);
    tw32_json_kv_uint("dropped",             s_state.dropped);
    tw32_json_kv_int ("channel",             s_state.channel);
    tw32_json_kv_bool("targeted",            s_state.have_target);
    if (s_state.have_target) tw32_json_kv_mac("target", s_state.target_bssid);
    tw32_json_kv_bool("can_export",          s_state.have_ssid);
    tw32_json_kv_bool("running",             s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "scan",   "list nearby APs (sets `target N` cache)", cmd_scan   },
    { "target", "target <N|bssid|any> (N keeps the ESSID)", cmd_target },
    { "chan",   "chan N (1..14)",                          cmd_chan   },
    { "start",  "begin capture",                           cmd_start  },
    { "stop",   "halt capture",                            cmd_stop   },
    { "stats",  "EAPOL counters + state",                  cmd_stats  },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();
    s_hs_q = xQueueCreate(HS_Q_LEN, sizeof(eapol_evt_t));
    configASSERT(s_hs_q != NULL);
    /* Priority 4 sits below the Wi-Fi RX task; core 1 keeps it off the
     * core the Wi-Fi/lwIP stack runs on. */
    xTaskCreatePinnedToCore(hs_drain_task, "tw32-hs-drain",
                            4096, NULL, 4, NULL, 1);
    wifi_init_promisc();
    tw32_wifi_scan_init();
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
