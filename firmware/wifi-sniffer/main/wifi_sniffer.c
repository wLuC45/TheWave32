/*
 * TheWave32 / wifi-sniffer
 *
 * Promiscuous-mode 802.11 capture, streaming PCAP records to UART0.
 *
 * Mode: WIFI_MODE_NULL + esp_wifi_set_promiscuous(true), no
 * channel-hopping (one channel sniffs cleanly; user picks via `chan`).
 * Snaplen is fixed at 256 B, which captures every management /
 * control frame in full and the headers + first payload bytes of
 * data frames. The link type is LINKTYPE_IEEE802_11_RADIOTAP (127),
 * so each record carries a small radiotap header (RSSI, channel,
 * FCS-at-end flag) before the 802.11 frame. Wireshark dissects this
 * directly.
 *
 * Data flow:
 *   1. Wi-Fi RX task → promiscuous callback → enqueue (drop on full)
 *   2. Drainer task on core 1 → write PCAP record to UART
 *
 * The CLI reads `start`/`stop`/`chan`/`stats` lines on UART RX. After
 * `start`, the host MUST switch to binary PCAP parsing on its end —
 * the firmware emits ack JSON, then the global header, then records
 * until `stop`, after which a final JSON ack is sent.
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

#define MODULE_NAME    "wifi-sniffer"
#define MODULE_VERSION "0.2.0"

#define SNAPLEN  256
#define QUEUE_LEN 64

/* PCAP classic file format magic numbers and header structs. */
#define PCAP_MAGIC          0xA1B2C3D4u
#define PCAP_VER_MAJOR      2
#define PCAP_VER_MINOR      4
/* LINKTYPE_IEEE802_11_RADIOTAP: each frame is prefixed with a radiotap
 * header so the capture carries RSSI and channel, which plain
 * LINKTYPE_IEEE802_11 (105) throws away. */
#define PCAP_LINKTYPE_RADIOTAP  127
/*
 * Radiotap body layout below: Flags (1) + 1 byte pad to align the
 * Channel field at offset 2 (radiotap requires Channel = u16+u16 at a
 * 2-byte-aligned offset) + Channel (4) + antsignal (1) = 7 bytes of
 * body, plus the 8-byte radiotap header = 15 bytes.
 */
#define RADIOTAP_LEN            15
/* IEEE80211_RADIOTAP_F_FCS = 0x10 in the Flags field: "frame includes
 * FCS". ESP-IDF's rx_ctrl.sig_len counts the trailing 4-byte FCS
 * (see esp_wifi_types_native.h), and we forward those bytes
 * verbatim, so we MUST advertise this to Wireshark or it will
 * mis-dissect the trailing 4 bytes as part of the 802.11 frame. */
#define RTAP_F_FCS              0x10
/* FCS length included in rx_ctrl.sig_len per ESP-IDF. */
#define IEEE80211_FCS_LEN       4

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t ver_major;
    uint16_t ver_minor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;
} pcap_global_t;

typedef struct __attribute__((packed)) {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} pcap_rec_hdr_t;

/* Minimal radiotap header. present bitmap selects Flags (bit 1),
 * Channel (bit 3) and Antenna signal in dBm (bit 5). Body field order
 * MUST match present-bit order. Channel requires 2-byte alignment
 * (radiotap.org), so a 1-byte pad sits between Flags and Channel. */
typedef struct __attribute__((packed)) {
    uint8_t  version;     /* 0 */
    uint8_t  pad;         /* 0 */
    uint16_t len;         /* RADIOTAP_LEN */
    uint32_t present;     /* (1<<1) Flags | (1<<3) Channel | (1<<5) dBm antsignal */
    uint8_t  flags;       /* RTAP_F_FCS set: frame includes trailing FCS */
    uint8_t  align_pad;   /* pad to align Channel at a 2-byte boundary */
    uint16_t chan_freq;   /* centre frequency in MHz */
    uint16_t chan_flags;  /* 0x00a0 = 2 GHz band */
    int8_t   antsignal;   /* dBm */
} radiotap_hdr_t;
_Static_assert(sizeof(radiotap_hdr_t) == RADIOTAP_LEN,
               "radiotap header layout mismatch");

typedef struct {
    uint32_t ts_us;       /* esp_timer_get_time low 32 bits */
    uint16_t orig_len;
    uint16_t incl_len;
    int8_t   rssi;        /* rx_ctrl.rssi, for the radiotap header */
    uint8_t  channel;     /* rx_ctrl.channel, for the radiotap header */
    uint8_t  data[SNAPLEN];
} sniff_rec_t;

typedef struct {
    volatile bool running;
    volatile uint8_t channel;
    /* captured (drain task) and dropped (Wi-Fi RX callback) are written on
     * one core and read by the CLI on another. Atomic so the cross-core
     * read/RMW is well defined rather than a data race. */
    _Atomic uint32_t captured;
    _Atomic uint32_t dropped;
} state_t;

static state_t s_state = { .running = false, .channel = 1, .captured = 0, .dropped = 0 };
static QueueHandle_t s_q;
/*
 * Single record reused by the cb to feed the queue. Queue copies the
 * struct in xQueueSend, so this static is safe — promisc cb is
 * single-threaded by ESP-IDF Wi-Fi RX.
 */
static sniff_rec_t s_rec;

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_state.running) {
        return;
    }
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    uint16_t orig = p->rx_ctrl.sig_len;
    uint16_t incl = orig < SNAPLEN ? orig : SNAPLEN;

    s_rec.ts_us    = (uint32_t)esp_timer_get_time();
    s_rec.orig_len = orig;
    s_rec.incl_len = incl;
    s_rec.rssi     = p->rx_ctrl.rssi;
    s_rec.channel  = p->rx_ctrl.channel;
    memcpy(s_rec.data, p->payload, incl);

    if (xQueueSend(s_q, &s_rec, 0) != pdTRUE) {
        s_state.dropped++;
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
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&promisc_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_state.channel, WIFI_SECOND_CHAN_NONE));
}

static void build_radiotap(radiotap_hdr_t *rt, int8_t rssi, uint8_t channel)
{
    rt->version = 0;
    rt->pad     = 0;
    rt->len     = RADIOTAP_LEN;
    /* Flags + Channel + dBm antsignal. */
    rt->present = (1u << 1) | (1u << 3) | (1u << 5);
    rt->flags     = RTAP_F_FCS;
    rt->align_pad = 0;
    /* Channel -> centre frequency mapping is only valid for 2.4 GHz
     * channels 1..14. The driver should never hand us anything outside
     * that range in 2 GHz promiscuous mode, but clamp defensively so a
     * bogus value cannot produce a negative or absurd frequency. */
    uint8_t ch = channel;
    if (ch < 1)  ch = 1;
    if (ch > 14) ch = 14;
    rt->chan_freq  = (ch == 14)
                     ? 2484
                     : (uint16_t)(2412 + (int)(ch - 1) * 5);
    rt->chan_flags = 0x00a0;               /* 2 GHz band */
    rt->antsignal  = rssi;
}

static void send_pcap_global(void)
{
    pcap_global_t g = {
        .magic     = PCAP_MAGIC,
        .ver_major = PCAP_VER_MAJOR,
        .ver_minor = PCAP_VER_MINOR,
        .thiszone  = 0,
        .sigfigs   = 0,
        .snaplen   = SNAPLEN + RADIOTAP_LEN,
        .network   = PCAP_LINKTYPE_RADIOTAP,
    };
    tw32_cdc_write(&g, sizeof(g));
}

static void drain_task(void *arg)
{
    (void)arg;
    sniff_rec_t rec;
    /* Single packed buffer for [pcap_rec_hdr][radiotap][frame bytes].
     * The shared UART tx mutex (tw32_cdc_write) only guarantees one
     * write call is atomic against other writers; splitting a PCAP
     * record across three calls lets a JSON ack from the CLI (e.g. a
     * `stats` reply while streaming) splice into the middle of the
     * record and desync the host's pcap parser. Sister fix:
     * firmware/wifi-probe-logger/main/wifi_probe_logger.c::emit_pcap. */
    static uint8_t pkt[sizeof(pcap_rec_hdr_t) + RADIOTAP_LEN + SNAPLEN];
    while (true) {
        if (xQueueReceive(s_q, &rec, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_state.running) {
            continue; /* drain stragglers without writing post-stop */
        }
        radiotap_hdr_t rt;
        build_radiotap(&rt, rec.rssi, rec.channel);
        /* Bound defensively (incl_len was already capped to SNAPLEN at
         * capture, but be explicit before the memcpy/length arithmetic). */
        uint16_t body = rec.incl_len > SNAPLEN ? SNAPLEN : rec.incl_len;
        pcap_rec_hdr_t h = {
            .ts_sec   = rec.ts_us / 1000000u,
            .ts_usec  = rec.ts_us % 1000000u,
            .incl_len = (uint32_t)RADIOTAP_LEN + body,
            .orig_len = (uint32_t)RADIOTAP_LEN + rec.orig_len,
        };
        size_t off = 0;
        memcpy(pkt + off, &h,  sizeof(h));  off += sizeof(h);
        memcpy(pkt + off, &rt, sizeof(rt)); off += sizeof(rt);
        memcpy(pkt + off, rec.data, body);  off += body;
        tw32_cdc_write(pkt, off);
        s_state.captured++;
    }
}

/* --- CLI handlers ---------------------------------------------------------- */

static int cmd_start(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    if (s_state.running) {
        tw32_cli_ack_ok("start");
        return 0;
    }
    tw32_cli_ack_ok("start");      /* JSON ack first */
    send_pcap_global();             /* then PCAP global header */
    s_state.running = true;          /* drain task starts emitting records */
    return 0;
}

static int cmd_stop(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    s_state.running = false;
    /* Queue may still hold records — drain task will discard them.
     * Brief delay lets the in-flight write complete. */
    vTaskDelay(pdMS_TO_TICKS(20));
    tw32_cli_ack_ok("stop");
    return 0;
}

static int cmd_chan(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) {
        tw32_cli_ack_err("chan", "missing_arg");
        return -1;
    }
    long ch = strtol(argv[1], NULL, 10);
    if (ch < 1 || ch > 14) {
        tw32_cli_ack_err("chan", "out_of_range");
        return -1;
    }
    if (esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        tw32_cli_ack_err("chan", "set_failed");
        return -1;
    }
    s_state.channel = (uint8_t)ch;
    tw32_cli_ack_ok("chan");
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("captured", s_state.captured);
    tw32_json_kv_uint("dropped",  s_state.dropped);
    tw32_json_kv_int ("channel",  s_state.channel);
    tw32_json_kv_bool("running",  s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start", "begin streaming PCAP",            cmd_start },
    { "stop",  "halt streaming",                  cmd_stop  },
    { "chan",  "chan N (1..13)",                  cmd_chan  },
    { "stats", "captured/dropped/channel/state",  cmd_stats },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();

    s_q = xQueueCreate(QUEUE_LEN, sizeof(sniff_rec_t));
    configASSERT(s_q != NULL);

    /* Optional persisted default channel via NVS. */
    uint32_t persisted = tw32_nvs_get_u32("sniff", "channel", 0);
    if (persisted >= 1 && persisted <= 13) {
        s_state.channel = (uint8_t)persisted;
    }

    wifi_init_promisc();

    /* Drain task on core 1 — the Wi-Fi driver lives on core 0, so this
     * keeps the UART writes off the RF-critical core. */
    xTaskCreatePinnedToCore(drain_task, "tw32-drain", 4096, NULL, 5, NULL, 1);

    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
