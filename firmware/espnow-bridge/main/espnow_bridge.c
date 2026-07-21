/*
 * TheWave32 / espnow-bridge
 *
 * ESP-NOW <-> UART bridge.
 *
 * Boots Wi-Fi in NULL mode, locks the radio to a chosen channel, and
 * registers ESP-NOW send + recv callbacks. Host-side text commands:
 *
 *   tx <hex>           — broadcast `hex` (decoded) on the broadcast peer
 *   txto <mac> <hex>   — unicast send to a specific peer (added on the
 *                        fly if not already registered)
 *   chan N             — set Wi-Fi channel 1..13
 *   stats              — counters
 *   start | stop       — toggle whether RX events are emitted
 *
 * On RX, when running, emit one JSON line per received frame with the
 * sender MAC, RSSI, and the payload (hex). Optional AES-CCM
 * encryption: if NVS namespace `espnow` has a 16-byte blob `aes_key`,
 * the broadcast peer is registered with that key (both ends must
 * share it).
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"

#define MODULE_NAME    "espnow-bridge"
#define MODULE_VERSION "0.2.0"

#define BROADCAST_MAC ((uint8_t[]){0xff, 0xff, 0xff, 0xff, 0xff, 0xff})
#define MAX_PAYLOAD   ESP_NOW_MAX_DATA_LEN /* 250 bytes */

/* Bounded queue between the ESP-NOW recv callback (Wi-Fi task context) and the
 * drain task that does the JSON emit + USB-CDC write. ESP-IDF docs are
 * explicit: "The receiving callback function also runs from the Wi-Fi task.
 * So, do not do lengthy operations in the callback function. Instead, post the
 * necessary data to a queue and handle it from a lower priority task."
 * (esp_now API reference). Taking the JSON mutex (portMAX_DELAY) and writing
 * to USB-CDC from the callback can stall the Wi-Fi task; route through a
 * queue. */
#define RX_QUEUE_LEN 8

typedef struct {
    uint32_t ts_ms;
    uint8_t  src[6];
    int8_t   rssi;
    uint16_t len;
    uint8_t  data[MAX_PAYLOAD];
} rx_evt_t;

typedef struct {
    volatile bool running;
    volatile uint8_t channel;
    /* tx_ok/tx_fail are written in the ESP-NOW send callback, rx_count in
     * the recv callback, all read by the CLI on another core: atomic. */
    _Atomic uint32_t tx_ok;
    _Atomic uint32_t tx_fail;
    _Atomic uint32_t rx_count;
    _Atomic uint32_t rx_dropped;  /* recv events lost to a full queue */
} state_t;

static state_t s_state = { .running = true, .channel = 1 };

static uint8_t s_aes_key[16];
static bool    s_have_key = false;

static QueueHandle_t s_rx_q;

/* --- helpers --------------------------------------------------------------- */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((int)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int hex_decode(const char *in, uint8_t *out, size_t cap)
{
    size_t n = strlen(in);
    if (n & 1) return -1;
    size_t bytes = n / 2;
    if (bytes > cap) return -1;
    for (size_t i = 0; i < bytes; ++i) {
        int hi = hex_nibble(in[2 * i]);
        int lo = hex_nibble(in[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)bytes;
}

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i]     = hex[in[i] >> 4];
        out[2 * i + 1] = hex[in[i] & 0x0f];
    }
    out[2 * n] = '\0';
}

static int parse_mac(const char *s, uint8_t mac[6])
{
    /* Accepts 12 hex chars or aa:bb:cc:dd:ee:ff. Validate length up front so
     * we never index past the NUL on short input (e.g. "aabb" would otherwise
     * read 12 bytes). */
    size_t slen = strlen(s);
    int sep;
    if (slen == 12)      sep = 0;
    else if (slen == 17) sep = 1;
    else                 return -1;
    for (int i = 0; i < 6; ++i) {
        const char *p = sep ? s + 3 * i : s + 2 * i;
        if (sep && i > 0 && *(p - 1) != ':') return -1;
        int hi = hex_nibble(p[0]);
        int lo = hex_nibble(p[1]);
        if (hi < 0 || lo < 0) return -1;
        mac[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static bool mac_is_broadcast(const uint8_t mac[6])
{
    for (int i = 0; i < 6; ++i) if (mac[i] != 0xff) return false;
    return true;
}

static esp_err_t ensure_peer(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }
    /* ESP-NOW broadcast peer cannot be encrypted; setting encrypt=true on
     * FF:FF:FF:FF:FF:FF makes esp_now_add_peer fail and aborts boot. */
    bool encrypt = s_have_key && !mac_is_broadcast(mac);
    esp_now_peer_info_t peer = {
        .channel = s_state.channel,
        .ifidx   = WIFI_IF_STA,
        .encrypt = encrypt,
    };
    memcpy(peer.peer_addr, mac, 6);
    if (encrypt) {
        memcpy(peer.lmk, s_aes_key, 16);
    }
    esp_err_t e = esp_now_add_peer(&peer);
    /* Race between is_peer_exist check and add_peer (or stale state from a
     * prior call): treat ALREADY_EXIST as success so the caller can proceed
     * to esp_now_send. */
    if (e == ESP_ERR_ESPNOW_EXIST) return ESP_OK;
    return e;
}

/* --- callbacks ------------------------------------------------------------- */

static void on_send(const uint8_t *mac, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_state.tx_ok++;
    } else {
        s_state.tx_fail++;
    }
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    s_state.rx_count++;
    if (!s_state.running) {
        return;
    }
    if (s_rx_q == NULL || len <= 0) {
        return;
    }
    /* Hand a fixed-size copy to the drain task. The recv callback runs in the
     * Wi-Fi task: do NOT take the JSON mutex or write USB-CDC here. A full
     * queue drops the event (counted) instead of stalling RX. The 500-char
     * hex buffer also stays out of the Wi-Fi task stack. */
    rx_evt_t ev;
    ev.ts_ms = (uint32_t)(esp_timer_get_time() / 1000);
    memcpy(ev.src, info->src_addr, 6);
    ev.rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    int n = (len > MAX_PAYLOAD) ? MAX_PAYLOAD : len;
    ev.len = (uint16_t)n;
    memcpy(ev.data, data, (size_t)n);
    if (xQueueSend(s_rx_q, &ev, 0) != pdTRUE) {
        s_state.rx_dropped++;
    }
}

static void rx_drain_task(void *arg)
{
    (void)arg;
    rx_evt_t ev;
    static char hex[MAX_PAYLOAD * 2 + 1]; /* keep off the task stack */
    for (;;) {
        if (xQueueReceive(s_rx_q, &ev, portMAX_DELAY) != pdTRUE) continue;
        /* Re-check running so an event in flight when `stop` arrived is
         * suppressed; rx_count was already bumped in the callback. */
        if (!s_state.running) continue;
        hex_encode(ev.data, ev.len, hex);
        tw32_json_begin();
        tw32_json_kv_str ("event", "rx");
        tw32_json_kv_uint("ts",    ev.ts_ms);
        tw32_json_kv_mac ("from",  ev.src);
        tw32_json_kv_int ("rssi",  ev.rssi);
        tw32_json_kv_int ("len",   ev.len);
        tw32_json_kv_str ("data",  hex);
        tw32_json_end();
    }
}

/* --- bring-up -------------------------------------------------------------- */

static void wifi_init_lr(void)
{
    { esp_err_t _e = esp_event_loop_create_default();
      if (_e != ESP_OK && _e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(_e); }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* WIFI_PROTOCOL_LR enables Espressif's LR mode for extra range. */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_state.channel, WIFI_SECOND_CHAN_NONE));
    esp_wifi_set_ps(WIFI_PS_NONE);
}

static void espnow_init(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_send));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

    /* If NVS has a 16-byte AES key, set it as the PMK so ALL peers
     * use it; otherwise transmissions are clear. */
    size_t klen = sizeof(s_aes_key);
    if (tw32_nvs_get_blob("espnow", "aes_key", s_aes_key, &klen) && klen == 16) {
        ESP_ERROR_CHECK(esp_now_set_pmk(s_aes_key));
        s_have_key = true;
    }

    /* Pre-register the broadcast peer. */
    ESP_ERROR_CHECK(ensure_peer(BROADCAST_MAC));

    /* RX path: callback -> queue -> drain task. Pin the drain off core 0 (the
     * Wi-Fi/CDC default) at a modest priority. 4 KB stack covers the static
     * hex buffer plus the JSON serializer. */
    s_rx_q = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_evt_t));
    configASSERT(s_rx_q != NULL);
    xTaskCreatePinnedToCore(rx_drain_task, "tw32-espnow-rx", 4096, NULL, 3, NULL, 1);
}

/* --- CLI ------------------------------------------------------------------- */

static int cmd_tx(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) {
        tw32_cli_ack_err("tx", "missing_arg");
        return -1;
    }
    uint8_t buf[MAX_PAYLOAD];
    int n = hex_decode(argv[1], buf, sizeof(buf));
    if (n <= 0) {
        tw32_cli_ack_err("tx", "bad_hex");
        return -1;
    }
    esp_err_t e = esp_now_send(BROADCAST_MAC, buf, (size_t)n);
    if (e != ESP_OK) {
        tw32_cli_ack_err("tx", "send_failed");
        return -1;
    }
    tw32_cli_ack_ok("tx");
    return 0;
}

static int cmd_txto(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 3) {
        tw32_cli_ack_err("txto", "missing_arg");
        return -1;
    }
    uint8_t mac[6];
    if (parse_mac(argv[1], mac) != 0) {
        tw32_cli_ack_err("txto", "bad_mac");
        return -1;
    }
    uint8_t buf[MAX_PAYLOAD];
    int n = hex_decode(argv[2], buf, sizeof(buf));
    if (n <= 0) {
        tw32_cli_ack_err("txto", "bad_hex");
        return -1;
    }
    if (ensure_peer(mac) != ESP_OK) {
        tw32_cli_ack_err("txto", "peer_add_failed");
        return -1;
    }
    if (esp_now_send(mac, buf, (size_t)n) != ESP_OK) {
        tw32_cli_ack_err("txto", "send_failed");
        return -1;
    }
    tw32_cli_ack_ok("txto");
    return 0;
}

static int cmd_chan(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("chan", "missing_arg"); return -1; }
    char *endp = NULL;
    long ch = strtol(argv[1], &endp, 10);
    /* Match the CLI help (1..13) and the boot-time NVS load range. */
    if (endp == argv[1] || *endp != '\0' || ch < 1 || ch > 13) {
        tw32_cli_ack_err("chan", "out_of_range");
        return -1;
    }
    if (esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        tw32_cli_ack_err("chan", "set_failed");
        return -1;
    }
    s_state.channel = (uint8_t)ch;
    /* Persist for next boot; boot reads espnow/channel and clamps to 1..13.
     * tw32_nvs_kv only exposes getters, so write directly via the IDF API. */
    nvs_handle_t h;
    if (nvs_open("espnow", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "channel", (uint32_t)ch);
        nvs_commit(h);
        nvs_close(h);
    }
    tw32_cli_ack_ok("chan");
    return 0;
}

static int cmd_start(tw32_cli_ctx_t *ctx, int argc, char **argv)
{ (void)ctx;(void)argc;(void)argv; s_state.running = true;  tw32_cli_ack_ok("start"); return 0; }

static int cmd_stop(tw32_cli_ctx_t *ctx, int argc, char **argv)
{ (void)ctx;(void)argc;(void)argv; s_state.running = false; tw32_cli_ack_ok("stop");  return 0; }

static int cmd_stats(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("tx_ok",   s_state.tx_ok);
    tw32_json_kv_uint("tx_fail", s_state.tx_fail);
    tw32_json_kv_uint("rx_count", s_state.rx_count);
    tw32_json_kv_uint("rx_dropped", s_state.rx_dropped);
    tw32_json_kv_int ("channel", s_state.channel);
    tw32_json_kv_bool("encrypted", s_have_key);
    tw32_json_kv_bool("running", s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "tx",    "tx <hex> — broadcast",                          cmd_tx    },
    { "txto",  "txto <mac> <hex> — unicast",                    cmd_txto  },
    { "chan",  "chan N (1..13)",                                cmd_chan  },
    { "start", "emit RX events to host",                        cmd_start },
    { "stop",  "suppress RX events (still counts in stats)",    cmd_stop  },
    { "stats", "tx_ok/tx_fail/rx_count/channel/encrypted",      cmd_stats },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();

    uint32_t persisted = tw32_nvs_get_u32("espnow", "channel", 0);
    if (persisted >= 1 && persisted <= 13) {
        s_state.channel = (uint8_t)persisted;
    }

    wifi_init_lr();
    espnow_init();

    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
