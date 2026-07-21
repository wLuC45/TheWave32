/*
 * TheWave32 / wifi-deauth-detector
 *
 * Defensive monitor: promiscuous-mode capture filtered to management
 * frames, looking for deauthentication (FC0 = 0xC0) and disassociation
 * (FC0 = 0xA0). Maintains a per-BSSID sliding-window counter and emits
 * `attack_alert` events when frame rate breaches the threshold.
 *
 * Broadcast deauth (source MAC 00:00:00:00:00:00 or FF:FF:FF:FF:FF:FF)
 * is flagged separately as it is the canonical signature of cheap
 * deauthers.
 *
 * Sliding window: small per-BSSID table (max 16 entries) of (bssid,
 * last_us, count) pairs; entries older than 1s reset their count. LRU
 * eviction at full.
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
#include "freertos/task.h"
#include "nvs_flash.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"

#define MODULE_NAME    "wifi-deauth-detector"
#define MODULE_VERSION "0.2.0"

#define FC0_DEAUTH    0xC0
#define FC0_DISASSOC  0xA0
#define WINDOW_US     1000000u
#define BSSID_SLOTS   16

typedef struct {
    bool     used;
    uint8_t  bssid[6];
    uint64_t last_us;          /* esp_timer is uint64; using uint32 wraps @ ~71 min */
    uint32_t count;
    bool     alerted;          /* debounce: only one alert per breach */
    uint16_t reason;           /* reason code of the window's first frame */
    bool     reason_uniform;   /* every frame this window shared `reason` */
} bssid_slot_t;

typedef struct {
    volatile bool     running;
    volatile bool     hopping;
    volatile uint8_t  channel;        /* current; written only by hopper_task */
    volatile uint8_t  req_channel;    /* desired channel when not hopping */
    volatile uint32_t dwell_ms;
    volatile uint32_t threshold;
    /* Written by the Wi-Fi RX callback, read by the CLI on another core. */
    _Atomic uint32_t  deauth_seen;
    _Atomic uint32_t  disassoc_seen;
    _Atomic uint32_t  alerts;
    bssid_slot_t      slots[BSSID_SLOTS];
} state_t;

static state_t s_state = {
    .channel = 1, .req_channel = 1, .dwell_ms = 250, .hopping = true, .threshold = 10,
};

static bssid_slot_t *find_or_alloc(const uint8_t *bssid, uint64_t now_us)
{
    /* Pass 1: scan ALL slots for an exact match. Breaking early on the
     * first free slot (prior version) could miss a real match later in
     * the table, silently splitting one BSSID across slots and breaking
     * the sliding window. */
    bssid_slot_t *free_slot = NULL;
    bssid_slot_t *lru = &s_state.slots[0];
    for (int i = 0; i < BSSID_SLOTS; i++) {
        if (s_state.slots[i].used &&
            memcmp(s_state.slots[i].bssid, bssid, 6) == 0) {
            return &s_state.slots[i];
        }
        if (!s_state.slots[i].used) {
            if (!free_slot) free_slot = &s_state.slots[i];
        } else if (s_state.slots[i].last_us < lru->last_us) {
            lru = &s_state.slots[i];
        }
    }
    bssid_slot_t *target = free_slot ? free_slot : lru;
    /* Reuse / claim slot. */
    memset(target, 0, sizeof(*target));
    target->used = true;
    memcpy(target->bssid, bssid, 6);
    target->last_us = now_us;
    return target;
}

static bool mac_zero(const uint8_t *m)
{
    for (int i = 0; i < 6; i++) if (m[i] != 0x00) return false;
    return true;
}

static bool mac_bcast(const uint8_t *m)
{
    for (int i = 0; i < 6; i++) if (m[i] != 0xFF) return false;
    return true;
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_state.running) return;
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *fr = p->payload;
    uint16_t flen = p->rx_ctrl.sig_len;
    if (flen < 24) return;

    bool is_deauth = (fr[0] == FC0_DEAUTH);
    bool is_disassoc = (fr[0] == FC0_DISASSOC);
    if (!is_deauth && !is_disassoc) return;

    if (is_deauth) s_state.deauth_seen++;
    else            s_state.disassoc_seen++;

    /* Reason code: the 2-byte frame body right after the 24-byte header. */
    uint16_t reason = (flen >= 26)
                      ? ((uint16_t)fr[24] | ((uint16_t)fr[25] << 8))
                      : 0;

    /* IEEE 802.11-2020 Fig. 9-2: addr1=RA/DA (offset 4), addr2=TA/SA
     * (offset 10), addr3=BSSID for mgmt (offset 16). */
    const uint8_t *bssid = fr + 16;
    const uint8_t *sa    = fr + 10;
    const uint8_t *da    = fr + 4;

    uint64_t now_us = (uint64_t)esp_timer_get_time();

    /* Canonical "broadcast deauth" attack (Bellardo & Savage, USENIX
     * 2003) sets DA = ff:ff:ff:ff:ff:ff so every associated STA is
     * kicked at once. The prior check inspected SA, which is wrong:
     * SA is the AP's MAC, never broadcast/zero in a real deauth. */
    if (is_deauth && (mac_bcast(da) || mac_zero(da))) {
        tw32_json_begin();
        tw32_json_kv_str ("event", "broadcast_deauth");
        tw32_json_kv_uint("ts",    now_us);
        tw32_json_kv_mac ("sa",    sa);
        tw32_json_kv_mac ("da",    da);
        tw32_json_kv_mac ("bssid", bssid);
        tw32_json_kv_int ("reason", reason);
        tw32_json_kv_int ("ch",    p->rx_ctrl.channel);
        tw32_json_end();
    }
    bssid_slot_t *slot = find_or_alloc(bssid, now_us);
    if ((now_us - slot->last_us) > WINDOW_US) {
        slot->count = 0;
        slot->alerted = false;
    }
    slot->last_us = now_us;
    /* The window's first frame seeds the reason code; any later frame
     * with a different code clears reason_uniform. A real flood keeps
     * one code; a busy network varies them, which lowers confidence. */
    if (slot->count == 0) {
        slot->reason = reason;
        slot->reason_uniform = true;
    } else if (reason != slot->reason) {
        slot->reason_uniform = false;
    }
    slot->count++;
    if (!slot->alerted && slot->count >= s_state.threshold) {
        slot->alerted = true;
        s_state.alerts++;
        tw32_json_begin();
        tw32_json_kv_str ("event", "attack_alert");
        tw32_json_kv_uint("ts",    now_us);
        tw32_json_kv_mac ("bssid", slot->bssid);
        tw32_json_kv_uint("count", slot->count);
        tw32_json_kv_uint("window_us", WINDOW_US);
        tw32_json_kv_int ("reason", slot->reason);
        tw32_json_kv_bool("reason_uniform", slot->reason_uniform);
        tw32_json_kv_int ("ch",    p->rx_ctrl.channel);
        tw32_json_end();
    }
}

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
    if (!strcmp(argv[1], "on"))  s_state.hopping = true;
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

static int cmd_threshold(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("threshold", "missing_arg"); return -1; }
    long n = strtol(argv[1], NULL, 10);
    if (n < 1 || n > 1000) { tw32_cli_ack_err("threshold", "out_of_range"); return -1; }
    s_state.threshold = (uint32_t)n;
    tw32_cli_ack_ok("threshold");
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("deauth_seen",   s_state.deauth_seen);
    tw32_json_kv_uint("disassoc_seen", s_state.disassoc_seen);
    tw32_json_kv_uint("alerts",        s_state.alerts);
    tw32_json_kv_uint("threshold",     s_state.threshold);
    tw32_json_kv_int ("channel",       s_state.channel);
    tw32_json_kv_uint("dwell_ms",      s_state.dwell_ms);
    tw32_json_kv_bool("hopping",       s_state.hopping);
    tw32_json_kv_bool("running",       s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start",     "begin monitoring",     cmd_start     },
    { "stop",      "halt monitoring",      cmd_stop      },
    { "chan",      "chan N (1..14)",       cmd_chan      },
    { "hop",       "hop on|off",           cmd_hop       },
    { "dwell",     "dwell N (50..5000)",   cmd_dwell     },
    { "threshold", "threshold N (1..1000)", cmd_threshold },
    { "stats",     "counters + state",     cmd_stats     },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();
    wifi_init_promisc();
    xTaskCreatePinnedToCore(hopper_task, "tw32-deauth-det", 2048, NULL, 3, NULL, 1);
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
