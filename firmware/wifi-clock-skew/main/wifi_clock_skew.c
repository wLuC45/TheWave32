/*
 * TheWave32 / wifi-clock-skew
 *
 * Per-AP clock-skew fingerprinting. Every 802.11 beacon carries an
 * 8-byte TSF timestamp: the AP's own microsecond clock. The rate at
 * which that clock drifts against ours is fixed by the AP's crystal
 * and is a stable hardware fingerprint, independent of SSID, BSSID and
 * channel.
 *
 * For each AP we collect (local_time, tsf_offset) pairs and run a
 * streaming least-squares regression (see skew_math.h). The slope is
 * the clock skew in ppm. The residual standard error of the fit is the
 * tell: a single AP yields a tight line; two transmitters spoofing one
 * BSSID produce two diverging offset lines and a large residual,
 * flagged as a `clone_suspect`.
 *
 * Technique: Kohno et al. 2005 (remote physical device
 * fingerprinting); Jana & Kasera 2008 (fake-AP detection by skew).
 *
 * Numerics. The regression uses the centred Welford/West update in
 * skew_math.h, which is stable on long captures (the old raw-power-sum
 * form lost precision to catastrophic cancellation). An optional
 * forgetting factor lambda lets the fit track a crystal whose skew
 * drifts with temperature; lambda == 1.0 (the default) is exact OLS.
 *
 * Detection. Clone flagging uses hysteresis (skew_math.h clone_det_t):
 * the residual must stay above the threshold for several emits before
 * the flag rises, and the flag clears when the AP settles. A single
 * delayed beacon no longer permanently mislabels an honest AP, and the
 * flag is no longer a one-way latch.
 *
 * Concurrency. The promiscuous RX callback only fills a fixed-size
 * record and enqueues it. One drain task owns the whole per-AP table,
 * runs the regression and emits JSON. No locks on the hot path, no JSON
 * or USB writes in the callback. Cross-context counters are C11 atomics
 * (the callback runs on the Wi-Fi core, the drain/CLI on core 1), and
 * the radio channel has a single writer (the hopper task) so pinning a
 * channel from the CLI can no longer race the hopper.
 */

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "skew_math.h"
#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"

#define MODULE_NAME    "wifi-clock-skew"
#define MODULE_VERSION "0.2.0"

#define FC0_BEACON      0x80
#define AP_SLOTS_PSRAM    512   /* APs tracked when PSRAM is available */
#define AP_SLOTS_FALLBACK  32   /* ...when it is not */
#define MIN_SAMPLES        20   /* beacons before the first skew estimate */
#define EMIT_EVERY         20   /* re-emit a refined estimate every N more */
#define BEACON_Q_LEN       64   /* deeper queue absorbs dense-channel bursts */
#define CLONE_US_DEFAULT  120   /* residual (us) above which a clone is flagged */
#define CLONE_STRIKES_SET   3   /* consecutive high-residual emits to flag */
#define CLONE_STRIKES_CLEAR 0   /* strikes at which the flag clears */
#define FORGET_MILLI_DEFAULT 1000 /* lambda*1000; 1000 == no forgetting (OLS) */

typedef struct {
    volatile bool     running;
    volatile bool     hopping;
    volatile uint8_t  channel;       /* current; written only by hopper_task */
    volatile uint8_t  req_channel;   /* desired channel when not hopping */
    /* Tunables written by CLI (core 1) and read by drain/hopper (core 1)
     * or the RX callback (Wi-Fi core). C11 `_Atomic` gives us the proper
     * cross-core read/write semantics; plain `volatile` is not a
     * synchronization primitive in C11. */
    _Atomic uint32_t  dwell_ms;
    _Atomic uint32_t  clone_us;       /* residual threshold for clone_suspect */
    _Atomic uint32_t  forget_milli;   /* lambda * 1000 */
    _Atomic uint32_t  beacons_seen;
    _Atomic uint32_t  aps_tracked;
    _Atomic uint32_t  skews_emitted;
    _Atomic uint32_t  clone_alerts;
    _Atomic uint32_t  dropped;
} state_t;

static state_t s_state = {
    .channel = 1, .req_channel = 1, .dwell_ms = 250, .hopping = true,
    .clone_us = CLONE_US_DEFAULT, .forget_milli = FORGET_MILLI_DEFAULT,
};

/* Record handed from the Wi-Fi RX callback to the drain task. */
typedef struct {
    uint64_t tsf;          /* AP clock from the beacon body */
    uint64_t local_us;     /* our receive time */
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  ssid_len;
    char     ssid[32];
} beacon_evt_t;

static QueueHandle_t s_q;

/* Per-AP regression state. Owned exclusively by the drain task. */
typedef struct {
    bool        used;
    uint8_t     bssid[6];
    char        ssid[33];
    uint8_t     channel;
    int8_t      rssi;
    uint64_t    tsf0;            /* origin: first beacon's TSF */
    uint64_t    local0;          /* origin: first beacon's local time */
    uint64_t    last_local;
    uint32_t    n;               /* samples accumulated since the origin */
    uint32_t    last_emit_n;
    skew_reg_t  reg;             /* streaming regression (skew_math.h) */
    clone_det_t clone;           /* clone hysteresis (skew_math.h) */
} ap_slot_t;

/* Heap-backed (PSRAM when present) and touched only by the drain task. */
static ap_slot_t *s_aps;
static int        s_ap_slots;
static uint32_t   s_used_count;     /* live AP count, drain-owned */

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_state.running) return;
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *fr = p->payload;
    uint16_t flen = p->rx_ctrl.sig_len;

    /* Need the 24-byte header, 12 bytes of fixed beacon params and at
     * least the 2-byte SSID IE header. */
    if (flen < 38) return;
    if (fr[0] != FC0_BEACON) return;

    beacon_evt_t ev;
    /* TSF is the first 8 bytes of the beacon body, little-endian on the
     * wire, same as the ESP32, so a plain copy is correct. */
    memcpy(&ev.tsf, fr + 24, 8);
    ev.local_us = (uint64_t)esp_timer_get_time();
    memcpy(ev.bssid, fr + 16, 6);          /* addr3 = BSSID */
    ev.rssi    = p->rx_ctrl.rssi;
    ev.channel = p->rx_ctrl.channel;
    /* First IE in a beacon is always the SSID (element id 0). */
    ev.ssid_len = 0;
    ev.ssid[0]  = '\0';
    if (fr[36] == 0) {
        uint8_t l = fr[37];
        if (l > 32) l = 32;
        if (38u + l <= flen) {
            memcpy(ev.ssid, fr + 38, l);
            ev.ssid_len = l;
        }
    }
    if (xQueueSend(s_q, &ev, 0) != pdTRUE) s_state.dropped++;
}

static ap_slot_t *find_or_alloc(const uint8_t *bssid)
{
    ap_slot_t *lru = &s_aps[0];
    for (int i = 0; i < s_ap_slots; i++) {
        if (s_aps[i].used && memcmp(s_aps[i].bssid, bssid, 6) == 0) {
            return &s_aps[i];
        }
        if (!s_aps[i].used) { lru = &s_aps[i]; break; }
        if (s_aps[i].last_local < lru->last_local) lru = &s_aps[i];
    }
    bool was_used = lru->used;
    memset(lru, 0, sizeof(*lru));
    lru->used = true;
    memcpy(lru->bssid, bssid, 6);
    if (!was_used) {
        /* A brand-new slot grew the live set. Eviction (reusing an
         * already-used slot) leaves the count unchanged, so this stays
         * O(1) instead of rescanning the whole table per beacon. */
        s_used_count++;
        s_state.aps_tracked = s_used_count;
    }
    return lru;
}

/* Re-seed a slot's regression origin (first beacon, or after a TSF
 * reset / AP reboot). */
static void slot_reorigin(ap_slot_t *s, const beacon_evt_t *ev)
{
    s->tsf0   = ev->tsf;
    s->local0 = ev->local_us;
    s->n      = 0;
    s->last_emit_n = 0;
    skew_reg_reset(&s->reg);
    clone_det_reset(&s->clone);
}

/* Clamp a double to a signed int range before casting. lround() of an
 * out-of-range double, or a cast that overflows the destination type,
 * is undefined behaviour in C99/C11 (6.3.1.4). Real crystals sit in
 * +/- 100 ppm, so any slope past +/- 1e6 ppm is either a transient
 * artefact (e.g. just after a TSF reorigin) or a NaN, and clipping it
 * costs nothing. */
static inline int32_t clamp_to_i32(double v)
{
    if (isnan(v))             return 0;
    if (v >  2147483600.0)    return  2147483600;
    if (v < -2147483600.0)    return -2147483600;
    return (int32_t)lround(v);
}

static void emit_skew(const ap_slot_t *s, double slope, double resid)
{
    /* skew exported as milli-ppm so it stays an integer (the JSON
     * emitter has no float field); the host divides by 1000. */
    int32_t mppm     = clamp_to_i32(slope * 1000.0);
    int32_t resid_us = clamp_to_i32(resid);
    tw32_json_begin();
    tw32_json_kv_str ("event",     "skew");
    tw32_json_kv_mac ("bssid",     s->bssid);
    tw32_json_kv_str ("ssid",      s->ssid);
    tw32_json_kv_int ("ch",        s->channel);
    tw32_json_kv_int ("rssi",      s->rssi);
    tw32_json_kv_int ("skew_mppm", mppm);
    tw32_json_kv_int ("resid_us",  resid_us);
    tw32_json_kv_uint("n",         s->n);
    tw32_json_end();
}

static void emit_clone(const ap_slot_t *s, const char *event, double resid)
{
    tw32_json_begin();
    tw32_json_kv_str ("event",    event);
    tw32_json_kv_mac ("bssid",    s->bssid);
    tw32_json_kv_str ("ssid",     s->ssid);
    tw32_json_kv_int ("ch",       s->channel);
    tw32_json_kv_int ("resid_us", clamp_to_i32(resid));
    tw32_json_kv_uint("n",        s->n);
    tw32_json_end();
}

static void hs_process(ap_slot_t *s)
{
    if (s->n < MIN_SAMPLES) return;
    if (s->n != MIN_SAMPLES && (s->n - s->last_emit_n) < EMIT_EVERY) return;
    s->last_emit_n = s->n;

    skew_fit_t f = skew_reg_fit(&s->reg);
    if (!f.valid) return;

    emit_skew(s, f.slope, f.resid);
    s_state.skews_emitted++;

    /* A tight line means one transmitter. A persistently large residual
     * means the (local, tsf) points do not lie on a single line, i.e.
     * more than one device is beaconing as this BSSID. Hysteresis keeps
     * a single outlier from flagging, and lets the flag clear. */
    int edge = clone_det_update(&s->clone, f.resid, (double)s_state.clone_us,
                                CLONE_STRIKES_SET, CLONE_STRIKES_CLEAR);
    if (edge > 0) {
        s_state.clone_alerts++;
        emit_clone(s, "clone_suspect", f.resid);
    } else if (edge < 0) {
        emit_clone(s, "clone_clear", f.resid);
    }
}

static void drain_task(void *arg)
{
    (void)arg;
    beacon_evt_t ev;
    for (;;) {
        if (xQueueReceive(s_q, &ev, portMAX_DELAY) != pdTRUE) continue;
        s_state.beacons_seen++;

        ap_slot_t *s = find_or_alloc(ev.bssid);
        s->channel = ev.channel;
        s->rssi    = ev.rssi;
        if (ev.ssid_len > 0) {
            memcpy(s->ssid, ev.ssid, ev.ssid_len);
            s->ssid[ev.ssid_len] = '\0';
        }

        if (s->n == 0 && s->local0 == 0) {
            slot_reorigin(s, &ev);
            s->last_local = ev.local_us;
            continue;
        }

        int64_t dtsf   = (int64_t)(ev.tsf - s->tsf0);
        int64_t dlocal = (int64_t)(ev.local_us - s->local0);
        /* TSF reset (AP reboot) or a wildly out-of-range offset: the
         * old line is dead, start a fresh one. */
        if (dtsf < 0 || dlocal < 0 ||
            llabs(dtsf - dlocal) > 100000000LL /* 100 s of drift */) {
            slot_reorigin(s, &ev);
            s->last_local = ev.local_us;
            continue;
        }

        double x = (double)dlocal / 1e6;        /* seconds */
        double y = (double)(dtsf - dlocal);     /* microseconds of drift */
        double lambda = (double)s_state.forget_milli / 1000.0;
        skew_reg_add(&s->reg, x, y, lambda);
        s->n++;
        s->last_local = ev.local_us;

        hs_process(s);
    }
}

static void hopper_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t delay = s_state.dwell_ms;
        if (s_state.running) {
            if (s_state.hopping) {
                uint8_t next = s_state.channel >= 13
                               ? 1 : (uint8_t)(s_state.channel + 1);
                if (esp_wifi_set_channel(next, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
                    s_state.channel = next;
                }
            } else if (s_state.channel != s_state.req_channel) {
                /* Apply a CLI pin request. The hopper is the sole writer
                 * of `channel` and the sole caller of set_channel during
                 * operation, so cmd_chan can never race this. Poll quickly
                 * while pinned so a pin takes effect within ~50 ms. */
                uint8_t want = s_state.req_channel;
                if (esp_wifi_set_channel(want, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
                    s_state.channel = want;
                }
                delay = 50;
            } else {
                delay = 50;   /* stay responsive to the next pin request */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

static void wifi_init_promisc(void)
{
    { esp_err_t _e = esp_event_loop_create_default();
      if (_e != ESP_OK && _e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(_e); }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&promisc_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_state.channel, WIFI_SECOND_CHAN_NONE));
}

/* --- CLI ---------------------------------------------------------------- */

static int cmd_start(tw32_cli_ctx_t *c, int argc, char **argv)
{ (void)c;(void)argc;(void)argv; s_state.running = true;  tw32_cli_ack_ok("start"); return 0; }

static int cmd_stop(tw32_cli_ctx_t *c, int argc, char **argv)
{ (void)c;(void)argc;(void)argv; s_state.running = false; tw32_cli_ack_ok("stop");  return 0; }

static int cmd_chan(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("chan", "missing_arg"); return -1; }
    long ch = strtol(argv[1], NULL, 10);
    if (ch < 1 || ch > 14) { tw32_cli_ack_err("chan", "out_of_range"); return -1; }
    /* Record the request; the hopper applies it (single writer of the
     * channel). Takes effect within one dwell interval. */
    s_state.req_channel = (uint8_t)ch;
    s_state.hopping = false;
    tw32_cli_ack_ok("chan");
    return 0;
}

static int cmd_hop(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("hop", "missing_arg"); return -1; }
    if (!strcmp(argv[1], "on")) {
        s_state.hopping = true;
    } else if (!strcmp(argv[1], "off")) {
        /* Freeze on the current channel rather than snapping back to
         * the last pinned request. Order matters: clear `hopping` first
         * so the hopper stops advancing `channel`, THEN snapshot it
         * into `req_channel`. The reverse order races with a hopper
         * tick: if the hopper advances `channel` between our read of
         * it and our write of `hopping=false`, we pin to the stale
         * value and the hopper snaps the radio back one step. */
        s_state.hopping = false;
        s_state.req_channel = s_state.channel;
    } else {
        tw32_cli_ack_err("hop", "use_on_or_off"); return -1;
    }
    tw32_cli_ack_ok("hop");
    return 0;
}

static int cmd_clone_us(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("clone_us", "missing_arg"); return -1; }
    long v = strtol(argv[1], NULL, 10);
    if (v < 10 || v > 100000) { tw32_cli_ack_err("clone_us", "out_of_range"); return -1; }
    s_state.clone_us = (uint32_t)v;
    tw32_cli_ack_ok("clone_us");
    return 0;
}

static int cmd_forget(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("forget", "missing_arg"); return -1; }
    long v = strtol(argv[1], NULL, 10);
    /* lambda in [0.9, 1.0]; 1000 disables forgetting (exact OLS). Lower
     * than 0.9 bounds the effective sample count 1/(1-lambda) below ~10,
     * too few for a stable fit. */
    if (v < 900 || v > 1000) { tw32_cli_ack_err("forget", "out_of_range"); return -1; }
    s_state.forget_milli = (uint32_t)v;
    tw32_cli_ack_ok("forget");
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("beacons_seen",  s_state.beacons_seen);
    tw32_json_kv_uint("aps_tracked",   s_state.aps_tracked);
    tw32_json_kv_uint("ap_slots",      (uint32_t)s_ap_slots);
    tw32_json_kv_uint("skews_emitted", s_state.skews_emitted);
    tw32_json_kv_uint("clone_alerts",  s_state.clone_alerts);
    tw32_json_kv_uint("dropped",       s_state.dropped);
    tw32_json_kv_int ("channel",       s_state.channel);
    tw32_json_kv_uint("clone_us",      s_state.clone_us);
    tw32_json_kv_uint("forget_milli",  s_state.forget_milli);
    tw32_json_kv_bool("hopping",       s_state.hopping);
    tw32_json_kv_bool("running",       s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start",    "begin fingerprinting",                cmd_start    },
    { "stop",     "halt fingerprinting",                 cmd_stop     },
    { "chan",     "chan N (1..14), pins the channel",    cmd_chan     },
    { "hop",      "hop on|off",                          cmd_hop      },
    { "clone_us", "clone_us N (residual threshold, us)", cmd_clone_us },
    { "forget",   "forget N (lambda*1000, 900..1000)",   cmd_forget   },
    { "stats",    "counters + state",                    cmd_stats    },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();

    /* Track APs in PSRAM when present so a dense environment (>32 APs)
     * does not churn slots and reset baselines: the slope's variance
     * falls as ~1/(n*Var(x)), so a long observation per AP matters. Fall
     * back to internal RAM if there is no PSRAM. */
    s_ap_slots = AP_SLOTS_PSRAM;
    s_aps = heap_caps_calloc(s_ap_slots, sizeof(ap_slot_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_aps == NULL) {
        s_ap_slots = AP_SLOTS_FALLBACK;
        s_aps = calloc(s_ap_slots, sizeof(ap_slot_t));
    }
    configASSERT(s_aps != NULL);

    s_q = xQueueCreate(BEACON_Q_LEN, sizeof(beacon_evt_t));
    configASSERT(s_q != NULL);
    /* Priority 4 sits below the Wi-Fi RX task; core 1 keeps the
     * double-precision regression off the Wi-Fi/lwIP core. */
    xTaskCreatePinnedToCore(drain_task,  "tw32-skew-drain", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(hopper_task, "tw32-skew-hop",   2048, NULL, 3, NULL, 1);
    wifi_init_promisc();
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
