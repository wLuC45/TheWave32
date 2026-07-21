/*
 * TheWave32 / wifi-mac-tracker
 *
 * Long-running observer that watches for specific MAC addresses
 * (configured via the CLI; persisted to NVS) and emits per-MAC
 * presence/absence events. Useful for "is this device near me right
 * now?" workflows - physical-presence sensing via Wi-Fi SAs.
 *
 * Per watched MAC the module keeps a small state machine:
 *
 *   ABSENT --(any frame seen)-->  PRESENT
 *   PRESENT --(no frames for `threshold_ms`)-->  ABSENT
 *
 * Each transition emits a JSON line so the host can react in real
 * time. RSSI is exposed as a rolling EMA so dashboards can plot signal
 * strength over time without us spamming events.
 *
 * The promiscuous filter is set to MGMT|DATA|CTRL - we want every
 * frame the device emits, including 802.11 control acks (TX visibility
 * even from busy phones).
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

#define MODULE_NAME    "wifi-mac-tracker"
#define MODULE_VERSION "0.2.0"

#define MAX_WATCH 16
#define LABEL_MAX 24
#define DEFAULT_THRESHOLD_MS 30000u

typedef struct {
    bool     used;
    uint8_t  mac[6];
    char     label[LABEL_MAX + 1];
    bool     present;
    bool     ema_init;       /* false until the first sample has been folded in */
    int8_t   last_rssi;
    int16_t  rssi_ema;       /* fixed-point: real_rssi * 4 */
    uint32_t last_seen_ms;
    uint32_t hits;
    /* Presence hysteresis: a single late frame after a long silence must
     * not flip ABSENT -> PRESENT instantly. We require either
     * `HYST_MIN_HITS` frames inside `HYST_WINDOW_MS`, or a single frame
     * within `HYST_CONT_MS` of the last seen frame (i.e. the device was
     * essentially still nearby and we just clipped through `threshold_ms`).
     */
    uint8_t  pending_hits;
    uint32_t pending_first_ms;
} watch_t;

#define HYST_MIN_HITS    2u
#define HYST_WINDOW_MS   3000u
#define HYST_CONT_MS     5000u

typedef struct {
    volatile bool     running;
    volatile bool     hopping;
    volatile uint8_t  channel;        /* current; written only by hopper_task */
    volatile uint8_t  req_channel;    /* desired channel when not hopping */
    volatile uint32_t dwell_ms;
    volatile uint32_t threshold_ms;
    /* Written by the Wi-Fi RX callback, read by the CLI on another core. */
    _Atomic uint32_t  total_seen;
    watch_t           list[MAX_WATCH];
    SemaphoreHandle_t lock;
} state_t;

static state_t s_state = {
    .channel = 1, .req_channel = 1, .dwell_ms = 250, .hopping = true,
    .threshold_ms = DEFAULT_THRESHOLD_MS,
};

#define LOCK()   xSemaphoreTake(s_state.lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_state.lock)

/* --- helpers ------------------------------------------------------- */

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

static int find_watch_locked(const uint8_t mac[6])
{
    for (int i = 0; i < MAX_WATCH; i++) {
        if (s_state.list[i].used && memcmp(s_state.list[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static int find_free_slot_locked(void)
{
    for (int i = 0; i < MAX_WATCH; i++) {
        if (!s_state.list[i].used) return i;
    }
    return -1;
}

/* --- NVS persistence (CSV string of mac=label entries) ------------- */

static void persist_locked(void)
{
    nvs_handle_t h;
    if (nvs_open("mactrk", NVS_READWRITE, &h) != ESP_OK) return;
    char buf[(17 + 1 + LABEL_MAX + 1) * MAX_WATCH + 1] = {0};
    size_t pos = 0;
    for (int i = 0; i < MAX_WATCH; i++) {
        if (!s_state.list[i].used) continue;
        if (pos > 0 && pos < sizeof(buf) - 1) buf[pos++] = ',';
        int n = snprintf(buf + pos, sizeof(buf) - pos,
                         "%02x:%02x:%02x:%02x:%02x:%02x=%s",
                         s_state.list[i].mac[0], s_state.list[i].mac[1],
                         s_state.list[i].mac[2], s_state.list[i].mac[3],
                         s_state.list[i].mac[4], s_state.list[i].mac[5],
                         s_state.list[i].label);
        if (n > 0) pos += (size_t)n;
    }
    nvs_set_str(h, "watch", buf);
    nvs_commit(h);
    nvs_close(h);
}

static void load_from_nvs(void)
{
    /* CSV grammar written by `persist_locked`:
     *   entry := "%02x:%02x:%02x:%02x:%02x:%02x" "=" label
     *   blob  := entry ("," entry)*
     * The classic two-line `strtok(buf, ",")` + `strchr(tok, '=')` parse
     * mis-handles two real cases:
     *   - a label containing ',' (the next entry is silently spliced into
     *     this one's label and dropped);
     *   - a label containing '=' (the first '=' wins, MAC parse fails,
     *     entry lost).
     * The MAC prefix is fixed-width (17 chars) and always followed by
     * exactly one '=', so we can split on position instead of on
     * delimiter content. Entries are bounded by the next ',' that
     * appears AFTER `LABEL_MAX` chars of label, or end-of-string. */
    char buf[(17 + 1 + LABEL_MAX + 1) * MAX_WATCH + 1] = {0};
    if (!tw32_nvs_get_str("mactrk", "watch", buf, sizeof(buf))) return;
    LOCK();
    size_t pos = 0;
    size_t len = strnlen(buf, sizeof(buf));
    while (pos < len) {
        /* Skip a leading separator, then require MAC + '=' + label. */
        while (pos < len && buf[pos] == ',') pos++;
        if (pos + 17 + 1 > len) break;
        if (buf[pos + 17] != '=') {
            /* Malformed entry: scan to next ',' and resync. */
            while (pos < len && buf[pos] != ',') pos++;
            continue;
        }
        char mac_str[18];
        memcpy(mac_str, buf + pos, 17);
        mac_str[17] = '\0';
        uint8_t mac[6];
        if (parse_mac(mac_str, mac) != 0) {
            while (pos < len && buf[pos] != ',') pos++;
            continue;
        }
        size_t label_start = pos + 18;
        size_t label_end   = label_start;
        size_t label_max_end = label_start + LABEL_MAX;
        if (label_max_end > len) label_max_end = len;
        /* The writer caps each label at LABEL_MAX chars, so any ','
         * encountered inside that window is the entry separator. After
         * LABEL_MAX chars the next ',' (or end-of-string) ends the
         * entry unconditionally. This also stops on stray '\0' bytes. */
        while (label_end < label_max_end &&
               buf[label_end] != '\0' &&
               buf[label_end] != ',') {
            label_end++;
        }
        char label[LABEL_MAX + 1];
        size_t llen = label_end - label_start;
        if (llen > LABEL_MAX) llen = LABEL_MAX;
        memcpy(label, buf + label_start, llen);
        label[llen] = '\0';

        int slot = find_free_slot_locked();
        if (slot >= 0) {
            memset(&s_state.list[slot], 0, sizeof(watch_t));
            s_state.list[slot].used = true;
            memcpy(s_state.list[slot].mac, mac, 6);
            memcpy(s_state.list[slot].label, label, llen);
            s_state.list[slot].label[llen] = '\0';
        }

        /* Advance past the label and any '=' / printable garbage that
         * would have been the tail of an over-long label, stopping at
         * the next ',' or end-of-string. */
        pos = label_end;
        while (pos < len && buf[pos] != ',') pos++;
    }
    UNLOCK();
}

/* --- promisc cb ---------------------------------------------------- */

static const uint8_t *frame_src_mac(const uint8_t *fr, uint8_t fc0)
{
    /* Frame Control byte 0 type/subtype determines header layout.
     * Mgmt (type 00) + most data (type 10): SA at offset 10.
     * Control (type 01): mostly no SA - skip those (CTS/ACK have only RA). */
    uint8_t type = (fc0 >> 2) & 0x3;
    if (type == 0x1) return NULL;          /* control frames - skip */
    return fr + 10;
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    if (!s_state.running) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    if (p->rx_ctrl.sig_len < 24) return;
    const uint8_t *src = frame_src_mac(p->payload, p->payload[0]);
    if (!src) return;
    /* Locally-administered or multicast - almost always not a tracking target. */
    if (src[0] & 0x01) return;

    s_state.total_seen++;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    int     rssi    = p->rx_ctrl.rssi;
    uint8_t ch_rx   = p->rx_ctrl.channel;

    LOCK();
    int idx = find_watch_locked(src);
    if (idx >= 0) {
        watch_t *w = &s_state.list[idx];
        w->hits++;
        uint32_t last_seen_prev = w->last_seen_ms;
        w->last_seen_ms = now_ms;
        w->last_rssi = (int8_t)rssi;
        /* EMA in fixed-point: store real_rssi * 4. Recurrence on the
         * real-valued ema is ema = (7*ema + new) / 8. Substituting
         * ema_fixed = 4*ema gives ema_fixed = (7*ema_fixed + 4*new) / 8,
         * which is what we compute below. The +-4 bias before the shift
         * gives symmetric round-to-nearest; without it C integer
         * division truncates toward zero and biases negative RSSI
         * upward (toward 0 dBm) by up to ~0.9 dBm per update.
         * First sample is folded in as the seed instead of decaying
         * down from 0 dBm, which otherwise produces a multi-second
         * transient at the top of the trace. */
        if (!w->ema_init) {
            w->rssi_ema = (int16_t)(4 * rssi);
            w->ema_init = true;
        } else {
            int32_t num  = 7 * (int32_t)w->rssi_ema + 4 * (int32_t)rssi;
            int32_t bias = (num >= 0) ? 4 : -4;
            w->rssi_ema  = (int16_t)((num + bias) / 8);
        }

        bool emit_entered = false;
        if (!w->present) {
            /* Hysteresis: take this frame as evidence of presence only
             * if (a) the previous sighting was recent enough that the
             * device probably never really left, or (b) we have seen
             * `HYST_MIN_HITS` frames inside `HYST_WINDOW_MS`. This
             * defeats the "one stray beacon flips ABSENT -> PRESENT"
             * flap-storm noted by Vanhoef et al. (AsiaCCS 2016, section
             * on randomized-MAC churn) where every new randomized SA
             * looks like a fresh sighting. */
            bool continuity = (last_seen_prev != 0) &&
                              ((now_ms - last_seen_prev) <= HYST_CONT_MS);
            if (w->pending_hits == 0 ||
                (now_ms - w->pending_first_ms) > HYST_WINDOW_MS) {
                w->pending_hits = 1;
                w->pending_first_ms = now_ms;
            } else {
                if (w->pending_hits < 0xFF) w->pending_hits++;
            }
            if (continuity || w->pending_hits >= HYST_MIN_HITS) {
                w->present = true;
                w->pending_hits = 0;
                emit_entered = true;
            }
        } else {
            w->pending_hits = 0;
        }

        if (emit_entered) {
            /* Snapshot label + transition-relevant fields under the
             * lock. Once we UNLOCK, a concurrent `remove`/`clear` can
             * memset this slot, so neither `w` nor `src` (which points
             * into a Wi-Fi RX buffer that the driver may recycle the
             * moment we return) can be touched after the unlock. */
            char    label[LABEL_MAX + 1];
            uint8_t mac_snap[6];
            strncpy(label, w->label, LABEL_MAX);
            label[LABEL_MAX] = '\0';
            memcpy(mac_snap, w->mac, 6);
            UNLOCK();
            tw32_json_begin();
            tw32_json_kv_str("event", "entered");
            tw32_json_kv_mac("mac", mac_snap);
            tw32_json_kv_str("label", label);
            tw32_json_kv_int("rssi", rssi);
            tw32_json_kv_int("ch", ch_rx);
            tw32_json_kv_uint("ts", now_ms);
            tw32_json_end();
            return;
        }
    }
    UNLOCK();
}

/* --- timeout sweeper task ----------------------------------------- */

static void sweep_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!s_state.running) continue;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t thresh = s_state.threshold_ms;
        LOCK();
        for (int i = 0; i < MAX_WATCH; i++) {
            watch_t *w = &s_state.list[i];
            if (!w->used || !w->present) continue;
            if ((now_ms - w->last_seen_ms) > thresh) {
                w->present = false;
                /* Reset the EMA seed and hysteresis counter so a device
                 * that comes back hours later re-seeds from its first
                 * fresh sample instead of decaying out of a stale value
                 * (or, worse, out of 0 dBm if `clear` had rebuilt the
                 * slot in the meantime). */
                w->ema_init = false;
                w->pending_hits = 0;
                /* Emit outside lock would be cleaner but the json_out lock
                 * serialises across tasks anyway, and the watch_t snapshot
                 * we'd take is small, so emit inside lock for simplicity. */
                tw32_json_begin();
                tw32_json_kv_str("event", "left");
                tw32_json_kv_mac("mac", w->mac);
                tw32_json_kv_str("label", w->label);
                tw32_json_kv_int("last_rssi", w->last_rssi);
                tw32_json_kv_uint("absent_ms", now_ms - w->last_seen_ms);
                tw32_json_kv_uint("ts", now_ms);
                tw32_json_end();
            }
        }
        UNLOCK();
    }
}

/* --- channel hopper ------------------------------------------------ */

static void hopper_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t delay = s_state.dwell_ms;
        if (s_state.running) {
            if (s_state.hopping) {
                uint8_t next = s_state.channel >= 13 ? 1 : (uint8_t)(s_state.channel + 1);
                if (esp_wifi_set_channel(next, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
                    s_state.channel = next;
                }
            } else if (s_state.channel != s_state.req_channel) {
                /* hopper is the single writer of `channel`; apply CLI pin. */
                uint8_t want = s_state.req_channel;
                if (esp_wifi_set_channel(want, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
                    s_state.channel = want;
                }
                delay = 50;
            } else {
                delay = 50;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

/* --- bring-up ------------------------------------------------------ */

static void wifi_init_promisc(void)
{
    { esp_err_t _e = esp_event_loop_create_default();
      if (_e != ESP_OK && _e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(_e); }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_DATA,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&promisc_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_state.channel, WIFI_SECOND_CHAN_NONE));
}

/* --- CLI ----------------------------------------------------------- */

static int cmd_add(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("add", "missing_arg"); return -1; }
    uint8_t mac[6];
    if (parse_mac(argv[1], mac) != 0) { tw32_cli_ack_err("add", "bad_mac"); return -1; }
    LOCK();
    if (find_watch_locked(mac) >= 0) {
        UNLOCK();
        tw32_cli_ack_err("add", "duplicate");
        return -1;
    }
    int slot = find_free_slot_locked();
    if (slot < 0) {
        UNLOCK();
        tw32_cli_ack_err("add", "list_full");
        return -1;
    }
    memset(&s_state.list[slot], 0, sizeof(watch_t));
    s_state.list[slot].used = true;
    memcpy(s_state.list[slot].mac, mac, 6);
    /* Optional label = remaining argv joined with spaces. Sanitize
     * ',' and '=' to '_' so the NVS CSV serialiser stays unambiguous
     * (label-with-',' was a real load_from_nvs splicing bug). */
    if (argc >= 3) {
        size_t pos = 0;
        for (int i = 2; i < argc && pos < LABEL_MAX; i++) {
            size_t L = strlen(argv[i]);
            size_t take = (pos + L > LABEL_MAX) ? LABEL_MAX - pos : L;
            for (size_t k = 0; k < take; k++) {
                char ch = argv[i][k];
                if (ch == ',' || ch == '=' || (unsigned char)ch < 0x20) ch = '_';
                s_state.list[slot].label[pos + k] = ch;
            }
            pos += take;
            if (i + 1 < argc && pos < LABEL_MAX) s_state.list[slot].label[pos++] = ' ';
        }
        s_state.list[slot].label[pos] = '\0';
    }
    persist_locked();
    UNLOCK();
    tw32_cli_ack_ok("add");
    return 0;
}

static int cmd_remove(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("remove", "missing_arg"); return -1; }
    uint8_t mac[6];
    if (parse_mac(argv[1], mac) != 0) { tw32_cli_ack_err("remove", "bad_mac"); return -1; }
    LOCK();
    int idx = find_watch_locked(mac);
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
    LOCK();
    static watch_t snap[MAX_WATCH];
    memcpy(snap, s_state.list, sizeof(snap));
    UNLOCK();
    int emitted = 0;
    for (int i = 0; i < MAX_WATCH; i++) {
        if (!snap[i].used) continue;
        emitted++;
        tw32_json_begin();
        tw32_json_kv_str("event", "watch");
        tw32_json_kv_int("idx", i);
        tw32_json_kv_mac("mac", snap[i].mac);
        tw32_json_kv_str("label", snap[i].label);
        tw32_json_kv_bool("present", snap[i].present);
        tw32_json_kv_int("last_rssi", snap[i].last_rssi);
        tw32_json_kv_int("rssi_ema", (int)(snap[i].rssi_ema / 4));
        tw32_json_kv_uint("hits", snap[i].hits);
        tw32_json_end();
    }
    tw32_json_begin();
    tw32_json_kv_str("cmd", "list");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_int("count", emitted);
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

static int cmd_threshold(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("threshold", "missing_arg"); return -1; }
    long ms = strtol(argv[1], NULL, 10);
    if (ms < 1000 || ms > 600000) { tw32_cli_ack_err("threshold", "out_of_range"); return -1; }
    s_state.threshold_ms = (uint32_t)ms;
    tw32_cli_ack_ok("threshold");
    return 0;
}

static int cmd_chan(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("chan", "missing_arg"); return -1; }
    long ch = strtol(argv[1], NULL, 10);
    if (ch < 1 || ch > 14) { tw32_cli_ack_err("chan", "out_of_range"); return -1; }
    /* hopper applies the pin within ~50 ms (single writer of channel). */
    s_state.req_channel = (uint8_t)ch;
    s_state.hopping = false;
    tw32_cli_ack_ok("chan");
    return 0;
}

static int cmd_hop(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("hop", "missing_arg"); return -1; }
    if (!strcmp(argv[1], "on"))       s_state.hopping = true;
    else if (!strcmp(argv[1], "off")) { s_state.req_channel = s_state.channel; s_state.hopping = false; }
    else { tw32_cli_ack_err("hop", "use_on_or_off"); return -1; }
    tw32_cli_ack_ok("hop");
    return 0;
}

static int cmd_dwell(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("dwell", "missing_arg"); return -1; }
    long ms = strtol(argv[1], NULL, 10);
    if (ms < 50 || ms > 5000) { tw32_cli_ack_err("dwell", "out_of_range"); return -1; }
    s_state.dwell_ms = (uint32_t)ms;
    tw32_cli_ack_ok("dwell");
    return 0;
}

static int cmd_start(tw32_cli_ctx_t *c, int argc, char **argv)
{ (void)c;(void)argc;(void)argv; s_state.running = true;  tw32_cli_ack_ok("start"); return 0; }

static int cmd_stop(tw32_cli_ctx_t *c, int argc, char **argv)
{ (void)c;(void)argc;(void)argv; s_state.running = false; tw32_cli_ack_ok("stop");  return 0; }

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    int watched = 0, present = 0;
    LOCK();
    for (int i = 0; i < MAX_WATCH; i++) {
        if (s_state.list[i].used) {
            watched++;
            if (s_state.list[i].present) present++;
        }
    }
    UNLOCK();
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("total_seen",  s_state.total_seen);
    tw32_json_kv_int ("watched",     watched);
    tw32_json_kv_int ("present",     present);
    tw32_json_kv_uint("threshold_ms",s_state.threshold_ms);
    tw32_json_kv_int ("channel",     s_state.channel);
    tw32_json_kv_uint("dwell_ms",    s_state.dwell_ms);
    tw32_json_kv_bool("hopping",     s_state.hopping);
    tw32_json_kv_bool("running",     s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "add",       "add <mac> [label]",            cmd_add       },
    { "remove",    "remove <mac>",                 cmd_remove    },
    { "list",      "dump watchlist + state",       cmd_list      },
    { "clear",     "purge watchlist",              cmd_clear     },
    { "threshold", "absent threshold ms (1k..600k)", cmd_threshold },
    { "chan",      "chan N (pin to channel)",      cmd_chan      },
    { "hop",       "hop on|off",                   cmd_hop       },
    { "dwell",     "dwell N ms (50..5000)",        cmd_dwell     },
    { "start",     "begin tracking",               cmd_start     },
    { "stop",      "halt tracking",                cmd_stop      },
    { "stats",     "counters + state",             cmd_stats     },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();
    s_state.lock = xSemaphoreCreateMutex();
    configASSERT(s_state.lock);
    load_from_nvs();
    wifi_init_promisc();
    xTaskCreatePinnedToCore(sweep_task,  "tw32-mactrk-sw",  3072, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(hopper_task, "tw32-mactrk-hop", 2048, NULL, 3, NULL, 1);
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
