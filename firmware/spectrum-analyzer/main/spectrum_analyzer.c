/*
 * TheWave32 / spectrum-analyzer
 *
 * Cheap 2.4 GHz Wi-Fi spectrum heatmap. Brings up the radio in
 * promiscuous mode but discards every received packet — we only
 * read rx_ctrl.rssi from the callback. The sweep task then hops
 * across channels 1..13, dwelling `dwell_ms` ms per channel and
 * accumulating min/avg/max RSSI from any energy that reaches the
 * receiver. After a full sweep, one JSON line is emitted with the
 * 13 per-channel summaries.
 *
 * BLE channel observation is not in v1: the controller does not
 * expose RSSI sampling on advertising channels (37/38/39) at user
 * level without significantly more code.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"

#define MODULE_NAME    "spectrum-analyzer"
#define MODULE_VERSION "0.2.0"

#define CH_MIN 1
#define CH_MAX 13
#define CH_COUNT (CH_MAX - CH_MIN + 1)

typedef struct {
    int32_t  samples;
    int32_t  rssi_sum;
    int8_t   rssi_min;
    int8_t   rssi_max;
} ch_stats_t;

typedef struct {
    volatile bool running;
    volatile uint32_t dwell_ms;
    /* Written by the sweep task, read by the CLI: atomic for safe access.
     * (The per-channel stats below are instead coordinated lock-free via
     * cur_idx parking, see promisc_cb / sweep_task.) */
    _Atomic uint32_t sweeps;
    /* Channel-range filter: scan only [range_lo .. range_hi]. Defaults
     * to the full 1..13 set; tightening narrows the dwell budget so
     * each remaining channel gets longer per-frame visibility. */
    volatile uint8_t  range_lo;
    volatile uint8_t  range_hi;
    /* Floor (dBm). Samples with rssi < floor are discarded - useful
     * to ignore noise / very-distant APs and concentrate on the
     * stronger neighbourhood. INT8_MIN means "accept all".
     * _Atomic so CLI updates publish cleanly to the IRAM cb on either
     * core (volatile alone is not a cross-core fence). */
    _Atomic int8_t    floor;
    ch_stats_t ch[CH_COUNT];
    /* cur_idx is the producer/consumer handshake between sweep_task
     * (sole writer) and the Wi-Fi RX cb (sole reader). _Atomic gives
     * acquire/release ordering across cores; volatile alone does not. */
    _Atomic uint8_t cur_idx;
} state_t;

static state_t s_state = {
    .running = false,
    .dwell_ms = 20,
    .sweeps = 0,
    .range_lo = CH_MIN,
    .range_hi = CH_MAX,
    .floor = INT8_MIN,
    .cur_idx = CH_COUNT,
};

/* cur_idx == CH_COUNT means "in transition / emitting" — drop samples so
 * they don't land in the wrong channel's slot or race with emit_sweep. */
#define IDX_PARKED CH_COUNT

static IRAM_ATTR void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    /*
     * Only sample RSSI; do not touch packet payloads. This callback
     * runs in the Wi-Fi RX path, so we keep it allocation-free and
     * minimal. We intentionally do not filter by `type` - control
     * frames also count as "energy".
     *
     * Concurrency: an acquire-load of cur_idx pairs with the sweep
     * task's release-store, so any prior reset_ch() writes for slot
     * `idx` are visible here before we mutate that slot.
     */
    uint8_t idx = atomic_load_explicit(&s_state.cur_idx, memory_order_acquire);
    if (idx >= CH_COUNT) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    int8_t rssi = p->rx_ctrl.rssi;
    int8_t fl = atomic_load_explicit(&s_state.floor, memory_order_relaxed);
    if (rssi < fl) return;
    ch_stats_t *cs = &s_state.ch[idx];
    cs->samples++;
    cs->rssi_sum += rssi;
    if (rssi < cs->rssi_min) cs->rssi_min = rssi;
    if (rssi > cs->rssi_max) cs->rssi_max = rssi;
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
    /*
     * Enable promiscuous mode with no filter — we want to see every
     * frame so the RSSI sample size is as large as possible per dwell.
     */
    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&promisc_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
}

static void reset_ch(uint8_t idx)
{
    s_state.ch[idx].samples = 0;
    s_state.ch[idx].rssi_sum = 0;
    s_state.ch[idx].rssi_min = INT8_MAX;
    s_state.ch[idx].rssi_max = INT8_MIN;
}

static void emit_sweep(uint32_t ts_ms, const ch_stats_t snap[CH_COUNT])
{
    tw32_json_begin();
    tw32_json_kv_str("event", "sweep");
    tw32_json_kv_uint("ts", ts_ms);
    tw32_json_kv_uint("dwell_ms", s_state.dwell_ms);
    tw32_json_array_begin("ch");
    for (int i = 0; i < CH_COUNT; ++i) {
        const ch_stats_t *c = &snap[i];
        /* Each channel record is a small JSON object. We hand-format
         * here because the json_out array helpers don't have nested
         * object support - keeping that out of v1 saves complexity.
         * Hand-formatted values are integers, so no escaping is needed
         * for the inner object. */
        char obj[80];
        /* Symmetric round-to-nearest with sign-aware bias.
         *
         * C99 6.5.5p6 mandates integer division truncates toward zero,
         * which biases the average of negative RSSI sums upward (toward
         * 0 dBm) by up to ~0.99 dBm. Apply +n/2 for non-negative sums
         * and -n/2 for negative sums so we round to the nearest integer.
         *
         * Invariant min <= avg <= max is preserved: the true mean of
         * the samples lies in [min, max] (both integer endpoints), and
         * rounding to the nearest integer cannot exit that interval.
         * Matches the wifi-mac-tracker EMA fix at
         * firmware/wifi-mac-tracker/main/wifi_mac_tracker.c:279. */
        int32_t avg = 0;
        if (c->samples > 0) {
            int32_t half = c->samples / 2;
            int32_t bias = (c->rssi_sum >= 0) ? half : -half;
            avg = (c->rssi_sum + bias) / c->samples;
        }
        int n = snprintf(obj, sizeof(obj),
                         "{\"ch\":%d,\"n\":%" PRId32
                         ",\"min\":%d,\"avg\":%" PRId32 ",\"max\":%d}",
                         CH_MIN + i,
                         c->samples,
                         c->samples > 0 ? c->rssi_min : 0,
                         avg,
                         c->samples > 0 ? c->rssi_max : 0);
        (void)n;
        /* The array_str helper escapes; we want raw JSON. The plot
         * client JSON-decodes the whole line and reads the per-channel
         * entries as strings, then JSON-decodes each entry: works for
         * v1 and avoids extending the json_out API with nested-object
         * support. */
        tw32_json_array_str(obj);
    }
    tw32_json_array_end();
    tw32_json_end();
}

static void sweep_task(void *arg)
{
    (void)arg;
    while (true) {
        if (!s_state.running) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        for (uint8_t i = 0; i < CH_COUNT && s_state.running; ++i) {
            /* Honour the channel range filter - channels outside the
             * window stay zeroed but get marked as "not sampled" by
             * having samples == 0 (consumer side already handles that). */
            uint8_t ch_id = CH_MIN + i;
            /* Park before clearing the slot so the cb cannot land a
             * write into ch[i] after reset_ch() zeroes it. Release
             * store: ensures reset_ch() writes that follow are
             * visible to the cb when it next loads cur_idx. */
            atomic_store_explicit(&s_state.cur_idx, IDX_PARKED,
                                  memory_order_release);
            reset_ch(i);
            if (ch_id < s_state.range_lo || ch_id > s_state.range_hi) {
                continue;
            }
            esp_wifi_set_channel(ch_id, WIFI_SECOND_CHAN_NONE);
            atomic_store_explicit(&s_state.cur_idx, i,
                                  memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(s_state.dwell_ms));
        }
        /* Park before emission so cb cannot mutate stats while we serialise. */
        atomic_store_explicit(&s_state.cur_idx, IDX_PARKED,
                              memory_order_release);
        if (s_state.running) {
            /* Drain any cb that already loaded a live cur_idx before
             * the park. The cb is tiny (a handful of instructions),
             * so a single tick (1 ms at CONFIG_FREERTOS_HZ=1000) is
             * ample slack; we use 2 ms to also cover the IDF Wi-Fi
             * task scheduling jitter. After this, the per-channel
             * stats are quiescent and safe to memcpy. */
            vTaskDelay(pdMS_TO_TICKS(2));
            ch_stats_t snap[CH_COUNT];
            memcpy(snap, s_state.ch, sizeof(snap));
            uint32_t ts = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            emit_sweep(ts, snap);
            atomic_fetch_add_explicit(&s_state.sweeps, 1,
                                      memory_order_relaxed);
        }
    }
}

static int cmd_start(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    s_state.running = true;
    tw32_cli_ack_ok("start");
    return 0;
}

static int cmd_stop(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    s_state.running = false;
    tw32_cli_ack_ok("stop");
    return 0;
}

static int cmd_dwell(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) {
        tw32_cli_ack_err("dwell", "missing_arg");
        return -1;
    }
    long ms = strtol(argv[1], NULL, 10);
    if (ms < 5 || ms > 5000) {
        tw32_cli_ack_err("dwell", "out_of_range");
        return -1;
    }
    s_state.dwell_ms = (uint32_t)ms;
    tw32_cli_ack_ok("dwell");
    return 0;
}

static int cmd_range(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 3) { tw32_cli_ack_err("range", "missing_arg"); return -1; }
    long lo = strtol(argv[1], NULL, 10);
    long hi = strtol(argv[2], NULL, 10);
    if (lo < CH_MIN || hi > CH_MAX || lo > hi) {
        tw32_cli_ack_err("range", "out_of_range"); return -1;
    }
    s_state.range_lo = (uint8_t)lo;
    s_state.range_hi = (uint8_t)hi;
    tw32_cli_ack_ok("range");
    return 0;
}

static int cmd_floor(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("floor", "missing_arg"); return -1; }
    if (!strcmp(argv[1], "any")) {
        atomic_store_explicit(&s_state.floor, INT8_MIN, memory_order_relaxed);
        tw32_cli_ack_ok("floor"); return 0;
    }
    long v = strtol(argv[1], NULL, 10);
    if (v < -127 || v > 0) { tw32_cli_ack_err("floor", "out_of_range"); return -1; }
    atomic_store_explicit(&s_state.floor, (int8_t)v, memory_order_relaxed);
    tw32_cli_ack_ok("floor");
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("sweeps",
                      atomic_load_explicit(&s_state.sweeps,
                                           memory_order_relaxed));
    tw32_json_kv_uint("dwell_ms", s_state.dwell_ms);
    tw32_json_kv_int ("range_lo", s_state.range_lo);
    tw32_json_kv_int ("range_hi", s_state.range_hi);
    tw32_json_kv_int ("floor",
                      atomic_load_explicit(&s_state.floor,
                                           memory_order_relaxed));
    tw32_json_kv_bool("running",  s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start", "begin sweeping",                     cmd_start },
    { "stop",  "halt sweeping",                      cmd_stop  },
    { "dwell", "dwell N (ms per channel; 5..5000)",  cmd_dwell },
    { "range", "range A B (1..13)",                  cmd_range },
    { "floor", "floor <dBm|any> drop weaker rssi",   cmd_floor },
    { "stats", "print counters",                     cmd_stats },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();
    wifi_init_promisc();

    /* Optional dwell from NVS so it survives flash. */
    uint32_t persisted = tw32_nvs_get_u32("specan", "dwell_ms", 0);
    if (persisted >= 5 && persisted <= 5000) {
        s_state.dwell_ms = persisted;
    }

    xTaskCreatePinnedToCore(sweep_task, "tw32-sweep", 4096, NULL, 4, NULL, 1);

    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
