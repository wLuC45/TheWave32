/*
 * TheWave32 / wifi-csi-collector
 *
 * Streams ESP-IDF Channel State Information (CSI) — the per-subcarrier
 * complex channel response — to UART. Useful for off-device motion
 * sensing, presence detection, gesture recognition, and other
 * RF-fingerprinting research that needs richer-than-RSSI signals.
 *
 * Wire format: after `start`, every CSI sample is emitted as a binary
 * record framed by a 4-byte magic + fixed-length header:
 *
 *   offset  size   field
 *   ------  ----   -------------------------------------------
 *      0      4    magic 0x54435349 ("TCSI", little-endian)
 *      4      4    ts_ms (uint32 LE)
 *      8      6    src MAC (big-endian, on-the-wire order)
 *     14      1    rssi (int8)
 *     15      1    channel (uint8, 1..14)
 *     16      1    sig_mode (0=non-HT, 1=HT, 3=VHT)
 *     17      1    bandwidth (0=20 MHz, 1=40 MHz)
 *     18      2    len (uint16 LE), CSI buffer length in bytes
 *     20    len    raw CSI data (pairs of int8 imaginary/real parts)
 *
 * Total envelope: 20 + len bytes. sig_mode and bandwidth are needed to
 * interpret the CSI vector: they fix how many subcarriers it holds.
 * Host parsers anchor on the magic; the rest is fixed offsets.
 *
 * `start` and `stop` round-trip ack JSON like wifi-sniffer; everything
 * between is binary.
 */

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

#define MODULE_NAME    "wifi-csi-collector"
#define MODULE_VERSION "0.2.0"

#define CSI_MAGIC 0x49534354u  /* 'I','S','C','T' little-endian = "TCSI" */
#define CSI_MAX_BYTES 384      /* HE LTF up to ~242 subcarriers × 2 bytes */

typedef struct {
    volatile bool     running;
    volatile uint8_t  channel;
    volatile bool     have_filter;
    uint8_t           filter_mac[6];
    volatile int8_t   rssi_min;        /* -127 = accept all */
    volatile uint16_t rate_limit_hz;   /* 0 = no limit */
    /* last_emit_ms has two writers (CSI RX cb and cmd_rate_limit), so it
     * needs to be _Atomic to avoid a torn read by either side. The RX cb
     * runs in the Wi-Fi task context (single producer for the time
     * window) and the CLI runs in its own task. */
    _Atomic uint32_t  last_emit_ms;
    /* samples_emitted is written by the drain task; the rest by the CSI RX
     * callback. All read by the CLI on another core: atomic for safe access. */
    _Atomic uint32_t  samples_emitted;
    _Atomic uint32_t  dropped;
    _Atomic uint32_t  out_of_band;
    _Atomic uint32_t  rssi_filtered;
    _Atomic uint32_t  rate_filtered;
} state_t;

static state_t s_state = { .channel = 6, .rssi_min = -127 };

/* Self-contained CSI sample handed from the RX callback to the drain
 * task. info->buf is only valid inside the callback, so the raw CSI
 * bytes are copied in. */
typedef struct {
    uint32_t ts_ms;
    uint8_t  mac[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  sig_mode;    /* 0=non-HT, 1=HT, 3=VHT */
    uint8_t  bandwidth;   /* 0=20 MHz, 1=40 MHz */
    uint16_t len;
    uint8_t  data[CSI_MAX_BYTES];
} csi_evt_t;

/* CSI is high-rate; a deeper queue absorbs USB back-pressure bursts.
 * 12 × ~400 B ≈ 4.8 KB of internal RAM. */
#define CSI_Q_LEN 12
static QueueHandle_t s_csi_q;

static void csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    if (!s_state.running || !info || !info->buf) return;
    if (info->len == 0 || info->len > CSI_MAX_BYTES) {
        atomic_fetch_add_explicit(&s_state.out_of_band, 1, memory_order_relaxed);
        return;
    }
    if (s_state.have_filter && memcmp(info->mac, s_state.filter_mac, 6) != 0) {
        return;
    }
    if ((int8_t)info->rx_ctrl.rssi < s_state.rssi_min) {
        atomic_fetch_add_explicit(&s_state.rssi_filtered, 1, memory_order_relaxed);
        return;
    }
    /* Snapshot rate_limit_hz once: the CLI can flip it to 0 between
     * the guard and the divide, which would be a divide-by-zero. */
    uint16_t hz = s_state.rate_limit_hz;
    if (hz > 0) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t period = 1000U / hz;
        uint32_t last   = atomic_load_explicit(&s_state.last_emit_ms,
                                               memory_order_relaxed);
        if (now_ms - last < period) {
            atomic_fetch_add_explicit(&s_state.rate_filtered, 1,
                                      memory_order_relaxed);
            return;
        }
        atomic_store_explicit(&s_state.last_emit_ms, now_ms,
                              memory_order_relaxed);
    }

    /* Copy the sample and enqueue: the blocking USB writes happen on
     * the drain task, never in the Wi-Fi/CSI RX context. */
    csi_evt_t ev;
    ev.ts_ms     = (uint32_t)(esp_timer_get_time() / 1000);
    memcpy(ev.mac, info->mac, 6);
    ev.rssi      = (int8_t)info->rx_ctrl.rssi;
    ev.channel   = (uint8_t)info->rx_ctrl.channel;
    ev.sig_mode  = (uint8_t)info->rx_ctrl.sig_mode;
    ev.bandwidth = (uint8_t)info->rx_ctrl.cwb;
    ev.len       = (uint16_t)info->len;
    memcpy(ev.data, info->buf, ev.len);
    if (xQueueSend(s_csi_q, &ev, 0) != pdTRUE) {
        atomic_fetch_add_explicit(&s_state.dropped, 1, memory_order_relaxed);
    }
}

/* Drain task: serialises each queued sample to USB-CDC. Priority sits
 * below the Wi-Fi RX task so a slow host throttles emission (queue
 * fills → dropped++) instead of stalling RX. */
static void csi_drain_task(void *arg)
{
    (void)arg;
    csi_evt_t ev;
    /* One contiguous buffer so the 20-byte header and the CSI payload
     * are emitted in a SINGLE tw32_cdc_write. tw32_cdc_write takes the
     * shared TX mutex per call, so two separate writes would let a JSON
     * status line (`stats`, `chan` ack, etc.) splice between the header
     * and the payload, corrupting the binary frame for the host parser. */
    uint8_t pkt[20 + CSI_MAX_BYTES];
    for (;;) {
        if (xQueueReceive(s_csi_q, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.len > CSI_MAX_BYTES) {
            /* Defensive: the cb already bounds-checks, but be loud
             * rather than smash the stack if that ever regresses. */
            atomic_fetch_add_explicit(&s_state.out_of_band, 1,
                                      memory_order_relaxed);
            continue;
        }
        uint32_t magic = CSI_MAGIC;
        memcpy(pkt + 0,  &magic,    4);
        memcpy(pkt + 4,  &ev.ts_ms, 4);
        memcpy(pkt + 8,  ev.mac,    6);
        pkt[14] = (uint8_t)ev.rssi;
        pkt[15] = ev.channel;
        pkt[16] = ev.sig_mode;
        pkt[17] = ev.bandwidth;
        memcpy(pkt + 18, &ev.len,   2);
        memcpy(pkt + 20, ev.data,   ev.len);
        tw32_cdc_write(pkt, (size_t)20 + ev.len);
        atomic_fetch_add_explicit(&s_state.samples_emitted, 1,
                                  memory_order_relaxed);
    }
}

static void wifi_init(void)
{
    { esp_err_t _e = esp_event_loop_create_default();
      if (_e != ESP_OK && _e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(_e); }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Promiscuous so we see every packet — CSI is reported per-RX. */
    wifi_promiscuous_filter_t pf = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&pf));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_state.channel, WIFI_SECOND_CHAN_NONE));

    /* CSI config + cb registration deferred to cmd_start to keep boot
     * panic-free if a future IDF rev changes csi_config fields — we'd
     * rather surface the error as a JSON ack than a reboot loop. */
}

static esp_err_t enable_csi_now(void)
{
    wifi_csi_config_t csi_cfg = {0};
    csi_cfg.lltf_en        = true;
    csi_cfg.htltf_en       = true;
    csi_cfg.stbc_htltf2_en = true;
    csi_cfg.ltf_merge_en   = true;
    esp_err_t err = esp_wifi_set_csi_config(&csi_cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_csi_rx_cb(&csi_rx_cb, NULL);
    if (err != ESP_OK) return err;
    return esp_wifi_set_csi(true);
}

/* --- CLI ----------------------------------------------------------- */

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

static int cmd_start(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    if (s_state.running) { tw32_cli_ack_ok("start"); return 0; }
    esp_err_t err = enable_csi_now();
    if (err != ESP_OK) {
        char buf[32];
        snprintf(buf, sizeof(buf), "csi_init=%d", err);
        tw32_cli_ack_err("start", buf);
        return -1;
    }
    /* Ack JSON BEFORE the binary stream starts so the host has a
     * marker to switch parsers. */
    tw32_cli_ack_ok("start");
    s_state.running = true;
    return 0;
}

static int cmd_stop(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    s_state.running = false;
    esp_wifi_set_csi(false);
    /* Let an in-flight callback finish enqueuing and the drain task
     * finish its current record, then drop anything still queued so no
     * binary bytes trail the JSON `stop` ack (the host flips parsers
     * on that ack). */
    vTaskDelay(pdMS_TO_TICKS(20));
    xQueueReset(s_csi_q);
    tw32_cli_ack_ok("stop");
    return 0;
}

static int cmd_chan(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
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

static int cmd_filter(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("filter", "missing_arg"); return -1; }
    if (!strcmp(argv[1], "any")) {
        s_state.have_filter = false;
        tw32_cli_ack_ok("filter");
        return 0;
    }
    uint8_t m[6];
    if (parse_mac(argv[1], m) != 0) {
        tw32_cli_ack_err("filter", "bad_mac"); return -1;
    }
    memcpy(s_state.filter_mac, m, 6);
    s_state.have_filter = true;
    tw32_cli_ack_ok("filter");
    return 0;
}

static int cmd_rssi_min(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("rssi_min", "missing_arg"); return -1; }
    long v = strtol(argv[1], NULL, 10);
    if (v < -127 || v > 0) { tw32_cli_ack_err("rssi_min", "out_of_range"); return -1; }
    s_state.rssi_min = (int8_t)v;
    tw32_cli_ack_ok("rssi_min");
    return 0;
}

static int cmd_rate_limit(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("rate_limit", "missing_arg"); return -1; }
    long hz = strtol(argv[1], NULL, 10);
    /* 0 disables the limiter; cap at 1000 Hz which is already past
     * what UART0 at 115 200 baud can sustain anyway. */
    if (hz < 0 || hz > 1000) { tw32_cli_ack_err("rate_limit", "out_of_range"); return -1; }
    s_state.rate_limit_hz = (uint16_t)hz;
    atomic_store_explicit(&s_state.last_emit_ms, 0, memory_order_relaxed);
    tw32_cli_ack_ok("rate_limit");
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("samples_emitted", s_state.samples_emitted);
    tw32_json_kv_uint("dropped",         s_state.dropped);
    tw32_json_kv_uint("out_of_band",     s_state.out_of_band);
    tw32_json_kv_uint("rssi_filtered",   s_state.rssi_filtered);
    tw32_json_kv_uint("rate_filtered",   s_state.rate_filtered);
    tw32_json_kv_int ("channel",         s_state.channel);
    tw32_json_kv_bool("filtered",        s_state.have_filter);
    if (s_state.have_filter) tw32_json_kv_mac("filter", s_state.filter_mac);
    tw32_json_kv_int ("rssi_min",        s_state.rssi_min);
    tw32_json_kv_int ("rate_limit_hz",   s_state.rate_limit_hz);
    tw32_json_kv_bool("running",         s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start",      "begin streaming CSI",                cmd_start      },
    { "stop",       "halt CSI stream",                    cmd_stop       },
    { "chan",       "chan N (1..14)",                     cmd_chan       },
    { "filter",     "filter <mac|any>",                   cmd_filter     },
    { "rssi_min",   "rssi_min N (-127..0; -127 = all)",   cmd_rssi_min   },
    { "rate_limit", "rate_limit Hz (0..1000; 0 = off)",   cmd_rate_limit },
    { "stats",      "samples + dropped + state",          cmd_stats      },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();
    s_csi_q = xQueueCreate(CSI_Q_LEN, sizeof(csi_evt_t));
    configASSERT(s_csi_q != NULL);
    /* Priority 4 sits below the Wi-Fi RX task; core 1 keeps it off the
     * core the Wi-Fi stack runs on. */
    xTaskCreatePinnedToCore(csi_drain_task, "tw32-csi-drain",
                            4096, NULL, 4, NULL, 1);
    wifi_init();
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
