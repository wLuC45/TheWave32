/*
 * TheWave32 / ble-scanner
 *
 * Passive BLE advertising scanner using the NimBLE host. On `start`,
 * configures continuous passive scanning (window == interval == 100 ms)
 * and emits one JSON line per advertisement received. Adv data is parsed
 * for the most useful fields (flags, name, services, manufacturer data,
 * tx power) before being shipped to the host on UART0.
 *
 * The NimBLE host runs on its own task (CONFIG_BT_NIMBLE_HOST_TASK).
 * GAP discovery callbacks fire on that task; we serialise the JSON
 * emission with the json_out lock used by the CLI on core 1.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_nimble_hci.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"

#define MODULE_NAME    "ble-scanner"
#define MODULE_VERSION "0.2.0"

#define NAME_FILTER_MAX 31

typedef struct {
    volatile bool running;
    /* Written in the NimBLE GAP callback, read by the CLI on another core:
     * atomic so the cross-core RMW/read is well defined. */
    _Atomic uint32_t adv_seen;
    _Atomic uint32_t adv_emitted;
    _Atomic uint32_t rssi_filtered;
    _Atomic uint32_t mac_filtered;
    _Atomic uint32_t name_filtered;
    volatile int8_t   rssi_min;          /* -127 = accept all */
    volatile bool     have_mac_filter;
    uint8_t           filter_mac[6];
    char              name_filter[NAME_FILTER_MAX + 1];
    volatile uint8_t  name_filter_len;   /* 0 = disabled */
} scanner_state_t;

static scanner_state_t s_state = { .rssi_min = -127 };

/*
 * Scan parameters: passive (no SCAN_REQ), no whitelist, continuous.
 * 100 ms window/interval gives 100% airtime per channel — fine on the
 * ESP32-S3 since BLE doesn't have to coexist with Wi-Fi here.
 */
static struct ble_gap_disc_params s_disc_params = {
    .itvl = 0x00A0,            /* 0xA0 * 0.625 ms = 100 ms */
    .window = 0x00A0,          /* same → continuous */
    .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
    .limited = 0,
    .passive = 1,
    .filter_duplicates = 0,    /* let host see all adv frames */
};

/* --- adv-data parsing ------------------------------------------------------ */

static const char *adv_event_to_str(uint8_t t)
{
    switch (t) {
        case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:      return "ADV_IND";
        case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:      return "ADV_DIRECT";
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:     return "ADV_SCAN";
        case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND:  return "ADV_NONCONN";
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP:     return "SCAN_RSP";
        default:                                  return "unknown";
    }
}

/* Hex-encode `data` (n bytes) into `key`. Returns the number of source
 * bytes actually encoded so the caller can flag truncation. The buffer
 * is sized for the practical legacy-adv payload (~31 B); extended-adv
 * mfg data can be longer than what fits, so callers should also report
 * the raw length when it matters. */
static size_t emit_hex(const char *key, const uint8_t *data, size_t n)
{
    char buf[80];
    size_t pos = 0;
    size_t encoded = 0;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n && pos + 3 <= sizeof(buf); ++i) {
        buf[pos++] = hex[data[i] >> 4];
        buf[pos++] = hex[data[i] & 0x0f];
        encoded++;
    }
    buf[pos] = '\0';
    tw32_json_kv_str(key, buf);
    return encoded;
}

/* Walk the AD structures and emit the fields we recognise. */
static void emit_adv_fields(const uint8_t *data, uint8_t len)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, data, len) != 0) {
        /* Mark the line so the host can tell "no AD types" apart
         * from "we received malformed AD". */
        tw32_json_kv_bool("parse_ok", false);
        return;
    }
    if (f.flags) {
        tw32_json_kv_uint("flags", f.flags);
    }
    if (f.name && f.name_len) {
        char name[32];
        size_t copy = (f.name_len < sizeof(name) - 1) ? f.name_len : sizeof(name) - 1;
        memcpy(name, f.name, copy);
        name[copy] = '\0';
        tw32_json_kv_str("name", name);
        if (f.name_is_complete) {
            tw32_json_kv_bool("name_complete", true);
        }
    }
    if (f.tx_pwr_lvl_is_present) {
        tw32_json_kv_int("tx_pwr", f.tx_pwr_lvl);
    }
    if (f.num_uuids16) {
        tw32_json_array_begin("svc16");
        for (int i = 0; i < f.num_uuids16; ++i) {
            tw32_json_array_int(f.uuids16[i].value);
        }
        tw32_json_array_end();
    }
    if (f.num_uuids128) {
        tw32_json_array_begin("svc128");
        for (int i = 0; i < f.num_uuids128; ++i) {
            char u[37];
            const uint8_t *b = f.uuids128[i].value;
            snprintf(u, sizeof(u),
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     b[15], b[14], b[13], b[12],
                     b[11], b[10], b[9], b[8],
                     b[7], b[6], b[5], b[4], b[3], b[2], b[1], b[0]);
            tw32_json_array_str(u);
        }
        tw32_json_array_end();
    }
    if (f.mfg_data_len > 0 && f.mfg_data) {
        size_t enc = emit_hex("mfg", f.mfg_data, f.mfg_data_len);
        /* Always report the source length so the host can detect
         * silent truncation when extended-adv mfg blobs exceed the
         * stack hex buffer (Apple Continuity, Microsoft swift-pair
         * etc. can push beyond the legacy 31-B envelope). */
        tw32_json_kv_uint("mfg_len", f.mfg_data_len);
        if (enc < f.mfg_data_len) {
            tw32_json_kv_bool("mfg_trunc", true);
        }
    }
}

/* --- GAP callback ---------------------------------------------------------- */

/* Returns true if the advertising payload contains a Complete- or
 * Shortened-Local-Name AD that contains `needle` (case-sensitive). */
static bool adv_name_matches(const uint8_t *data, uint8_t len,
                             const char *needle, uint8_t needle_len)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, data, len) != 0) return false;
    if (!f.name || !f.name_len || needle_len == 0) return false;
    /* memmem is GNU; emulate. */
    for (int i = 0; i + needle_len <= f.name_len; ++i) {
        if (memcmp(f.name + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }
    const struct ble_gap_disc_desc *d = &event->disc;

    s_state.adv_seen++;

    uint8_t mac_be[6];
    for (int i = 0; i < 6; ++i) {
        mac_be[i] = d->addr.val[5 - i];
    }

    /* --- filters ------------------------------------------------- */
    if (s_state.have_mac_filter && memcmp(mac_be, s_state.filter_mac, 6) != 0) {
        s_state.mac_filtered++;
        return 0;
    }
    if ((int8_t)d->rssi < s_state.rssi_min) {
        s_state.rssi_filtered++;
        return 0;
    }
    /* Snapshot the gate once: the writer (cmd_name) may flip len=0
     * then back to >0 mid-check, and we want a consistent read of
     * the same length we use for the compare. */
    uint8_t nlen = s_state.name_filter_len;
    if (nlen > 0) {
        if (!adv_name_matches(d->data, d->length_data,
                              s_state.name_filter, nlen)) {
            s_state.name_filtered++;
            return 0;
        }
    }

    s_state.adv_emitted++;
    tw32_json_begin();
    tw32_json_kv_str("event", "adv");
    tw32_json_kv_uint("ts", (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    tw32_json_kv_str("type", adv_event_to_str(d->event_type));
    tw32_json_kv_uint("addr_type", d->addr.type);
    tw32_json_kv_mac("mac", mac_be);
    tw32_json_kv_int("rssi", d->rssi);
    if (d->length_data > 0 && d->data) {
        emit_adv_fields(d->data, d->length_data);
    }
    tw32_json_end();

    return 0;
}

/* --- NimBLE bring-up ------------------------------------------------------- */

static void on_sync(void)
{
    /*
     * Triggered after the controller is up. We don't auto-start
     * scanning; the user controls it via the `start` command. The
     * sync callback is required by NimBLE; without it the host
     * won't accept scan-start requests.
     */
}

static void on_reset(int reason)
{
    /* If the controller resets we just give up scanning until
     * the next `start`. */
    s_state.running = false;
}

static void host_task(void *param)
{
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

/* --- CLI handlers ---------------------------------------------------------- */

static int cmd_start(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    if (s_state.running) {
        tw32_cli_ack_ok("start");
        return 0;
    }
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &s_disc_params,
                          gap_event_cb, NULL);
    /* BLE_HS_EALREADY means a discovery is already running in the
     * controller (e.g. our running flag got cleared by on_reset but
     * the controller did not actually stop). Adopt it as success and
     * resync the running flag rather than wedging the CLI. */
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        char err[24];
        snprintf(err, sizeof(err), "ble_gap_disc=%d", rc);
        tw32_cli_ack_err("start", err);
        return -1;
    }
    s_state.running = true;
    tw32_cli_ack_ok("start");
    return 0;
}

static int cmd_stop(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    if (s_state.running) {
        ble_gap_disc_cancel();
        s_state.running = false;
    }
    tw32_cli_ack_ok("stop");
    return 0;
}

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

static int cmd_filter(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("filter", "missing_arg"); return -1; }
    if (!strcmp(argv[1], "any")) {
        s_state.have_mac_filter = false;
        tw32_cli_ack_ok("filter");
        return 0;
    }
    uint8_t m[6];
    if (parse_mac(argv[1], m) != 0) {
        tw32_cli_ack_err("filter", "bad_mac"); return -1;
    }
    /* Gate-off / write / gate-on so the NimBLE GAP callback (which
     * reads have_mac_filter then filter_mac without a lock) cannot
     * observe a torn 6-byte MAC mid-update. Same pattern as
     * wifi-probe-logger:cmd_filter. */
    s_state.have_mac_filter = false;
    memcpy(s_state.filter_mac, m, 6);
    s_state.have_mac_filter = true;
    tw32_cli_ack_ok("filter");
    return 0;
}

static int cmd_rssi_min(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("rssi_min", "missing_arg"); return -1; }
    long v = strtol(argv[1], NULL, 10);
    if (v < -127 || v > 0) { tw32_cli_ack_err("rssi_min", "out_of_range"); return -1; }
    s_state.rssi_min = (int8_t)v;
    tw32_cli_ack_ok("rssi_min");
    return 0;
}

static int cmd_name(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("name", "missing_arg"); return -1; }
    if (!strcmp(argv[1], "any")) {
        s_state.name_filter_len = 0;
        s_state.name_filter[0] = '\0';
        tw32_cli_ack_ok("name");
        return 0;
    }
    size_t L = strlen(argv[1]);
    if (L == 0 || L > NAME_FILTER_MAX) {
        tw32_cli_ack_err("name", "bad_length"); return -1;
    }
    /* Same gate-off / write / gate-on dance as cmd_filter. The GAP
     * callback gates on name_filter_len > 0 before reading the bytes,
     * so zeroing the length first blocks the reader until we have
     * the full new substring in place. */
    s_state.name_filter_len = 0;
    memcpy(s_state.name_filter, argv[1], L);
    s_state.name_filter[L] = '\0';
    s_state.name_filter_len = (uint8_t)L;
    tw32_cli_ack_ok("name");
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("adv_seen",      s_state.adv_seen);
    tw32_json_kv_uint("adv_emitted",   s_state.adv_emitted);
    tw32_json_kv_uint("rssi_filtered", s_state.rssi_filtered);
    tw32_json_kv_uint("mac_filtered",  s_state.mac_filtered);
    tw32_json_kv_uint("name_filtered", s_state.name_filtered);
    tw32_json_kv_int ("rssi_min",      s_state.rssi_min);
    tw32_json_kv_bool("filtered",      s_state.have_mac_filter);
    if (s_state.have_mac_filter) tw32_json_kv_mac("filter", s_state.filter_mac);
    if (s_state.name_filter_len > 0) tw32_json_kv_str("name_filter", s_state.name_filter);
    tw32_json_kv_bool("running",       s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start",    "begin BLE passive scan",         cmd_start    },
    { "stop",     "halt BLE scan",                  cmd_stop     },
    { "filter",   "filter <mac|any> by MAC",        cmd_filter   },
    { "rssi_min", "rssi_min N (-127..0)",           cmd_rssi_min },
    { "name",     "name <substr|any>",              cmd_name     },
    { "stats",    "print counters",                 cmd_stats    },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();
    ble_init();

    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
