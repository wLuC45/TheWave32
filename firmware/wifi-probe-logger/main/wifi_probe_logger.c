/*
 * TheWave32 / wifi-probe-logger (v0.2)
 *
 * Passive 802.11 probe-request logger with device-side fingerprinting.
 *
 * Pipeline:
 *   promisc cb (Wi-Fi RX, core 0)
 *     │  filter type==MGMT && FC0==0x40 (probe-req) && rssi>=rssi_min
 *     ▼
 *   xQueue (128 slots × ~280 B fixed records)
 *     ▼
 *   drain task (core 1)
 *     ├── parse IEs (SSID + rates + HT/VHT/HE + ext caps + vendor OUIs)
 *     ├── compute fingerprint = CRC32 over (rates ‖ ext_rates ‖ HT cap ‖
 *     │                            VHT cap ‖ HE flag ‖ ext caps ‖ sorted OUIs)
 *     ├── update per-MAC peer table (LRU eviction at 64 entries)
 *     └── emit:
 *           pcap=on  → raw record (link-type 105) on UART
 *           pcap=off → JSON line with everything parsed + fp + vendors
 *
 * Channel hopping: cycles 1..13 by default, dwell `dwell_ms` per channel
 * (configurable 50..5000). `hop off` + `chan N` for pinned ops.
 *
 * What this gives you that the v1 didn't:
 *   - Stable fingerprint across MAC randomisation (CRC32 of capability
 *     IEs in canonical order). Two probes from the same physical
 *     device burst share an `fp` even if the `src` rotates.
 *   - Vendor-OUI list extracted from id=221 IEs (Apple/Microsoft/Broadcom
 *     etc.) — useful for fleet identification.
 *   - On-device peer aggregation (`peers` command) so the host doesn't
 *     have to deduplicate.
 *   - Optional PCAP mode for Wireshark workflows.
 *   - RSSI floor + queue HWM observability.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"

#define MODULE_NAME    "wifi-probe-logger"
#define MODULE_VERSION "0.3.0"

/* 802.11 management frame: ProtoVer=00 Type=00(mgmt) Subtype=0100(probe-req)
 * → byte 0 == 0x40. */
#define FC0_PROBE_REQ 0x40

/* IE IDs we care about. */
#define IE_SSID         0
#define IE_RATES        1
#define IE_EXT_RATES   50
#define IE_HT_CAPS     45
#define IE_VHT_CAPS   191
#define IE_EXT_CAPS   127
#define IE_EXTENSION  255   /* len byte 0 carries an Extension ID */
#define EXT_ID_HE_CAPS 35
#define IE_VENDOR     221   /* first 3 bytes = OUI */

#define SNAPLEN     256
#define QUEUE_LEN   128
/* Peer table grows into PSRAM when present so a dense site (an airport,
 * a conference) does not churn the LRU and lose long-lived peers; falls
 * back to internal RAM otherwise. */
#define PEER_SLOTS_PSRAM    256
#define PEER_SLOTS_FALLBACK  64
#define MAX_OUIS     16

/* PCAP classic (host-byte-order, magic 0xa1b2c3d4 → little-endian fields). */
#define PCAP_MAGIC          0xA1B2C3D4u
#define PCAP_VER_MAJOR      2
#define PCAP_VER_MINOR      4
#define PCAP_LINKTYPE_IEEE80211 105

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

/* Queue record holds the raw frame; the drain task does all parsing. */
typedef struct {
    uint32_t ts_us;
    int8_t   rssi;
    uint8_t  channel;
    uint16_t orig_len;
    uint16_t incl_len;
    uint8_t  data[SNAPLEN];
} sniff_rec_t;

typedef struct {
    bool     used;
    uint8_t  src[6];
    uint32_t first_us;
    uint32_t last_us;
    uint32_t count;
    uint32_t fp;
    int8_t   last_rssi;
} peer_t;

typedef struct {
    volatile bool     running;
    volatile bool     hopping;
    volatile bool     pcap_mode;
    volatile uint8_t  channel;        /* current; written only by hopper_task */
    volatile uint8_t  req_channel;    /* desired channel when not hopping */
    volatile uint32_t dwell_ms;
    volatile int8_t   rssi_min;     /* accept rssi >= rssi_min; -127 = any */
    volatile bool     have_mac_filter;
    uint8_t           filter_mac[6];
    volatile bool     have_oui_filter;
    uint8_t           filter_oui[3]; /* 24-bit OUI prefix */
    /* Counters written on the Wi-Fi RX core (dropped/mac_filtered/
     * oui_filtered) or the drain core (total/directed/broadcast/hwm) and
     * read by the CLI: atomic so the cross-core access is well defined. */
    _Atomic uint32_t total;
    _Atomic uint32_t directed;
    _Atomic uint32_t broadcast;
    _Atomic uint32_t dropped;
    _Atomic uint32_t mac_filtered;
    _Atomic uint32_t oui_filtered;
    _Atomic uint32_t hwm;
    _Atomic uint16_t peers_used;
} state_t;

static state_t s_state = {
    .running = false,
    .hopping = true,
    .pcap_mode = false,
    .channel = 1,
    .req_channel = 1,
    .dwell_ms = 250,
    .rssi_min = -127,
};

static QueueHandle_t s_q;
/* Heap-backed (PSRAM when present); slot count chosen at boot. */
static peer_t *s_peers;
static int     s_peer_slots;
/* Snapshot buffer used by cmd_peers; sized to s_peer_slots so the dump
 * is a true snapshot taken under the lock and the heavy iteration runs
 * lock-free. Heap-backed (PSRAM when present) so an 8 KB worst-case
 * table does not pin internal DRAM. */
static peer_t *s_peer_snap;
/* Guards s_peers + peers_used: the drain task updates the table while
 * the CLI task snapshots or clears it. The promisc RX callback never
 * touches s_peers (it only enqueues), so this lock is never taken in
 * ISR/RX context. */
static SemaphoreHandle_t s_peer_lock;

/* --- IE walker ----------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    uint8_t  len;
} ie_t;

typedef struct {
    ie_t   ssid;
    ie_t   rates;
    ie_t   ext_rates;
    ie_t   ht;
    ie_t   vht;
    ie_t   ext_caps;
    bool   he;
    uint8_t ouis[MAX_OUIS * 3];
    uint8_t oui_count;
} parsed_t;

static void parse_ies(const uint8_t *ies, uint16_t total, parsed_t *out)
{
    /* IEEE 802.11 element format: id(1) | len(1) | body(len). Every
     * `len` byte must be checked against the buffer remaining BEFORE
     * we advance, including the two header bytes. Arithmetic is widened
     * to uint32_t so a pathological (id, 0xFF) at the tail cannot wrap
     * a uint16_t computation. */
    memset(out, 0, sizeof(*out));
    uint32_t off = 0;
    uint32_t cap = total;
    while (off + 2u <= cap) {
        uint8_t  id  = ies[off];
        uint8_t  len = ies[off + 1];
        uint32_t end = off + 2u + (uint32_t)len;
        if (end > cap) break;
        const uint8_t *body = ies + off + 2u;
        switch (id) {
            case IE_SSID:      out->ssid.p = body; out->ssid.len = len; break;
            case IE_RATES:     out->rates.p = body; out->rates.len = len; break;
            case IE_EXT_RATES: out->ext_rates.p = body; out->ext_rates.len = len; break;
            case IE_HT_CAPS:   out->ht.p = body; out->ht.len = len; break;
            case IE_VHT_CAPS:  out->vht.p = body; out->vht.len = len; break;
            case IE_EXT_CAPS:  out->ext_caps.p = body; out->ext_caps.len = len; break;
            case IE_EXTENSION:
                if (len >= 1 && body[0] == EXT_ID_HE_CAPS) out->he = true;
                break;
            case IE_VENDOR:
                if (len >= 3 && out->oui_count < MAX_OUIS) {
                    memcpy(&out->ouis[out->oui_count * 3], body, 3);
                    out->oui_count++;
                }
                break;
        }
        off = end;
    }
}

/* Sort OUIs in place (lex by 3 bytes) so fingerprint is independent
 * of vendor IE order. Insertion sort — n is at most MAX_OUIS=16. */
static void sort_ouis(uint8_t *ouis, uint8_t n)
{
    for (int i = 1; i < n; i++) {
        uint8_t key[3] = { ouis[i*3], ouis[i*3+1], ouis[i*3+2] };
        int j = i - 1;
        while (j >= 0 && memcmp(&ouis[j*3], key, 3) > 0) {
            memcpy(&ouis[(j+1)*3], &ouis[j*3], 3);
            j--;
        }
        memcpy(&ouis[(j+1)*3], key, 3);
    }
}

static uint32_t compute_fp(const parsed_t *p)
{
    uint8_t ouis_sorted[MAX_OUIS * 3];
    memcpy(ouis_sorted, p->ouis, p->oui_count * 3);
    sort_ouis(ouis_sorted, p->oui_count);

    uint32_t crc = 0;
    /* Guard NULL pointers: an absent IE has p==NULL and len==0; passing NULL
     * to esp_rom_crc32_le with len=0 is undefined and crashes on some builds. */
    if (p->rates.len)     crc = esp_rom_crc32_le(crc, p->rates.p,     p->rates.len);
    if (p->ext_rates.len) crc = esp_rom_crc32_le(crc, p->ext_rates.p, p->ext_rates.len);
    if (p->ht.len)        crc = esp_rom_crc32_le(crc, p->ht.p,        p->ht.len);
    if (p->vht.len)       crc = esp_rom_crc32_le(crc, p->vht.p,       p->vht.len);
    uint8_t he_flag = p->he ? 1 : 0;
    crc = esp_rom_crc32_le(crc, &he_flag, 1);
    if (p->ext_caps.len)  crc = esp_rom_crc32_le(crc, p->ext_caps.p,  p->ext_caps.len);
    if (p->oui_count)     crc = esp_rom_crc32_le(crc, ouis_sorted,    p->oui_count * 3);
    return crc;
}

/* --- peer table ---------------------------------------------------------- */

static peer_t *peer_find(const uint8_t mac[6])
{
    for (int i = 0; i < s_peer_slots; i++) {
        if (s_peers[i].used && memcmp(s_peers[i].src, mac, 6) == 0) {
            return &s_peers[i];
        }
    }
    return NULL;
}

static peer_t *peer_alloc(const uint8_t mac[6], uint32_t now_us)
{
    for (int i = 0; i < s_peer_slots; i++) {
        if (!s_peers[i].used) {
            memset(&s_peers[i], 0, sizeof(peer_t));
            s_peers[i].used = true;
            memcpy(s_peers[i].src, mac, 6);
            s_peers[i].first_us = now_us;
            s_state.peers_used++;
            return &s_peers[i];
        }
    }
    /* LRU evict. */
    peer_t *lru = &s_peers[0];
    for (int i = 1; i < s_peer_slots; i++) {
        if (s_peers[i].last_us < lru->last_us) lru = &s_peers[i];
    }
    memset(lru, 0, sizeof(peer_t));
    lru->used = true;
    memcpy(lru->src, mac, 6);
    lru->first_us = now_us;
    return lru;
}

static void peer_update(const uint8_t mac[6], uint32_t now_us, int8_t rssi, uint32_t fp)
{
    xSemaphoreTake(s_peer_lock, portMAX_DELAY);
    peer_t *pe = peer_find(mac);
    if (!pe) pe = peer_alloc(mac, now_us);
    pe->last_us = now_us;
    pe->count++;
    pe->fp = fp;
    pe->last_rssi = rssi;
    xSemaphoreGive(s_peer_lock);
}

/* --- promisc callback (RX path) ----------------------------------------- */

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_state.running) return;
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = p->payload;
    uint16_t flen = p->rx_ctrl.sig_len;
    if (flen < 24) return;
    if (frame[0] != FC0_PROBE_REQ) return;
    if ((int8_t)p->rx_ctrl.rssi < s_state.rssi_min) return;
    /* Source MAC lives at offset 10..15 in a probe-request. */
    const uint8_t *src_mac = frame + 10;
    if (s_state.have_mac_filter &&
        memcmp(src_mac, s_state.filter_mac, 6) != 0) {
        s_state.mac_filtered++;
        return;
    }
    if (s_state.have_oui_filter &&
        memcmp(src_mac, s_state.filter_oui, 3) != 0) {
        s_state.oui_filtered++;
        return;
    }

    sniff_rec_t r;
    r.ts_us    = (uint32_t)esp_timer_get_time();
    r.rssi     = p->rx_ctrl.rssi;
    r.channel  = p->rx_ctrl.channel;
    r.orig_len = flen;
    r.incl_len = flen < SNAPLEN ? flen : SNAPLEN;
    memcpy(r.data, frame, r.incl_len);

    if (xQueueSend(s_q, &r, 0) != pdTRUE) {
        s_state.dropped++;
    }
}

/* --- emission: JSON ------------------------------------------------------ */

static void hexcat(const uint8_t *src, size_t n, char *out)
{
    static const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2*i]   = hex[src[i] >> 4];
        out[2*i+1] = hex[src[i] & 0x0f];
    }
    out[2*n] = '\0';
}

static void emit_json(const sniff_rec_t *r, const parsed_t *p, uint32_t fp)
{
    /* Pre-render hex blobs on stack; sized for worst-case IE lengths. */
    char rates_hex[2*32+1]     = "";
    char extr_hex [2*32+1]     = "";
    char ht_hex   [2*64+1]     = "";
    char vht_hex  [2*32+1]     = "";
    char ec_hex   [2*32+1]     = "";
    if (p->rates.len)     hexcat(p->rates.p, p->rates.len > 32 ? 32 : p->rates.len, rates_hex);
    if (p->ext_rates.len) hexcat(p->ext_rates.p, p->ext_rates.len > 32 ? 32 : p->ext_rates.len, extr_hex);
    if (p->ht.len)        hexcat(p->ht.p, p->ht.len > 64 ? 64 : p->ht.len, ht_hex);
    if (p->vht.len)       hexcat(p->vht.p, p->vht.len > 32 ? 32 : p->vht.len, vht_hex);
    if (p->ext_caps.len)  hexcat(p->ext_caps.p, p->ext_caps.len > 32 ? 32 : p->ext_caps.len, ec_hex);

    /* SA at frame offset 10. */
    const uint8_t *src = &r->data[10];

    tw32_json_begin();
    tw32_json_kv_str ("event", "probe");
    tw32_json_kv_uint("ts",    r->ts_us);
    tw32_json_kv_mac ("src",   src);
    tw32_json_kv_int ("rssi",  r->rssi);
    tw32_json_kv_int ("ch",    r->channel);
    /* 802.11 sequence number (top 12 bits of the Sequence Control field
     * at frame offset 22): probes from one device in a burst carry
     * monotonic, close seq numbers, so they group even when the source
     * MAC is randomised. `randomized` flags a locally-administered MAC
     * (bit 1 of the first octet), separating real MACs from random ones. */
    uint16_t seq_ctrl = (uint16_t)r->data[22] | ((uint16_t)r->data[23] << 8);
    tw32_json_kv_int ("seq",        seq_ctrl >> 4);
    tw32_json_kv_bool("randomized", (src[0] & 0x02) != 0);
    if (p->ssid.len > 0) {
        char s[33];
        uint8_t copy = p->ssid.len > 32 ? 32 : p->ssid.len;
        memcpy(s, p->ssid.p, copy);
        s[copy] = '\0';
        tw32_json_kv_str ("ssid",      s);
        tw32_json_kv_bool("broadcast", false);
    } else {
        tw32_json_kv_bool("broadcast", true);
    }
    if (rates_hex[0]) tw32_json_kv_str("rates",     rates_hex);
    if (extr_hex [0]) tw32_json_kv_str("ext_rates", extr_hex);
    if (ht_hex   [0]) tw32_json_kv_str("ht",        ht_hex);
    if (vht_hex  [0]) tw32_json_kv_str("vht",       vht_hex);
    tw32_json_kv_bool("he", p->he);
    if (ec_hex[0])    tw32_json_kv_str("ext_caps", ec_hex);
    if (p->oui_count > 0) {
        tw32_json_array_begin("vendors");
        for (int i = 0; i < p->oui_count; i++) {
            char o[9];
            const uint8_t *b = &p->ouis[i*3];
            snprintf(o, sizeof(o), "%02x:%02x:%02x", b[0], b[1], b[2]);
            tw32_json_array_str(o);
        }
        tw32_json_array_end();
    }
    char fp_hex[11];
    snprintf(fp_hex, sizeof(fp_hex), "%08" PRIx32, fp);
    tw32_json_kv_str("fp", fp_hex);
    tw32_json_end();
}

static void emit_pcap(const sniff_rec_t *r)
{
    /* Pack header + body in one buffer so the per-call UART tx mutex
     * keeps the record atomic: a JSON line written from another task
     * between header and body would desync any pcap reader. SNAPLEN
     * caps incl_len, so this buffer is bounded. */
    uint8_t pkt[sizeof(pcap_rec_hdr_t) + SNAPLEN];
    pcap_rec_hdr_t h = {
        .ts_sec   = r->ts_us / 1000000u,
        .ts_usec  = r->ts_us % 1000000u,
        .incl_len = r->incl_len,
        .orig_len = r->orig_len,
    };
    uint16_t body = r->incl_len > SNAPLEN ? SNAPLEN : r->incl_len;
    memcpy(pkt, &h, sizeof(h));
    memcpy(pkt + sizeof(h), r->data, body);
    tw32_cdc_write(pkt, sizeof(h) + body);
}

/* --- drain task --------------------------------------------------------- */

static void drain_task(void *arg)
{
    (void)arg;
    sniff_rec_t r;
    parsed_t parsed;
    while (true) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) continue;

        /* HWM: depth at the moment we just dequeued, +1 to count this one. */
        uint32_t depth = uxQueueMessagesWaiting(s_q) + 1;
        if (depth > s_state.hwm) s_state.hwm = depth;

        s_state.total++;

        /* Parse IEs always (cheap; needed for peer table). */
        parse_ies(&r.data[24], r.incl_len > 24 ? r.incl_len - 24 : 0, &parsed);
        uint32_t fp = compute_fp(&parsed);

        if (parsed.ssid.len == 0) s_state.broadcast++;
        else                       s_state.directed++;

        peer_update(&r.data[10], r.ts_us, r.rssi, fp);

        if (s_state.pcap_mode) {
            emit_pcap(&r);
        } else {
            emit_json(&r, &parsed, fp);
        }
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

/* --- bring-up ----------------------------------------------------------- */

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

static int cmd_start(tw32_cli_ctx_t *ctx, int argc, char **argv)
{ (void)ctx;(void)argc;(void)argv;
  if (s_state.pcap_mode && !s_state.running) {
      pcap_global_t g = { .magic=PCAP_MAGIC, .ver_major=PCAP_VER_MAJOR,
                          .ver_minor=PCAP_VER_MINOR, .thiszone=0, .sigfigs=0,
                          .snaplen=SNAPLEN, .network=PCAP_LINKTYPE_IEEE80211 };
      tw32_cli_ack_ok("start");
      tw32_cdc_write(&g, sizeof(g));
  } else {
      tw32_cli_ack_ok("start");
  }
  s_state.running = true;
  return 0; }

static int cmd_stop(tw32_cli_ctx_t *ctx, int argc, char **argv)
{ (void)ctx;(void)argc;(void)argv;
  s_state.running = false;
  vTaskDelay(pdMS_TO_TICKS(20));   /* let the in-flight write finish */
  tw32_cli_ack_ok("stop");
  return 0; }

static int cmd_chan(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("chan", "missing_arg"); return -1; }
    long ch = strtol(argv[1], NULL, 10);
    if (ch < 1 || ch > 14) { tw32_cli_ack_err("chan", "out_of_range"); return -1; }
    /* hopper applies the pin within ~50 ms (single writer of channel). */
    s_state.req_channel = (uint8_t)ch;
    s_state.hopping = false;
    tw32_cli_ack_ok("chan");
    return 0;
}

static int cmd_hop(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("hop", "missing_arg"); return -1; }
    if      (!strcmp(argv[1], "on"))  s_state.hopping = true;
    else if (!strcmp(argv[1], "off")) { s_state.req_channel = s_state.channel; s_state.hopping = false; }
    else { tw32_cli_ack_err("hop", "use_on_or_off"); return -1; }
    tw32_cli_ack_ok("hop");
    return 0;
}

static int cmd_dwell(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("dwell", "missing_arg"); return -1; }
    long ms = strtol(argv[1], NULL, 10);
    if (ms < 50 || ms > 5000) { tw32_cli_ack_err("dwell", "out_of_range"); return -1; }
    s_state.dwell_ms = (uint32_t)ms;
    tw32_cli_ack_ok("dwell");
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

static int parse_mac(const char *s, uint8_t mac[6])
{
    /* Strict 'aa:bb:cc:dd:ee:ff' form: 17 chars, colons at fixed
     * positions, every other position is one hex digit. sscanf("%x")
     * is permissive about width, so we validate every character first
     * and pin each pair to exactly two hex digits via "%2x". */
    if (!s || strlen(s) != 17) return -1;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (s[i] != ':') return -1;
        } else {
            char c = s[i];
            if (!((c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return -1;
        }
    }
    int v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
               &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) return -1;
    for (int i = 0; i < 6; i++) {
        if (v[i] < 0 || v[i] > 0xFF) return -1;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

static int parse_oui(const char *s, uint8_t oui[3])
{
    /* Accept 'aa:bb:cc' or 'aabbcc'. sscanf("%2x") consumes 1-2 hex
     * digits, so 'aabbcz' would parse as (aa, bb, 0c) and silently
     * accept garbage; reject any non-hex char explicitly before
     * scanning. The 24-bit OUI registry uses fixed pairs (see
     * https://standards-oui.ieee.org/), so we mirror that. */
    if (!s) return -1;
    int v[3];
    size_t n = strlen(s);
    if (n == 8 && s[2] == ':' && s[5] == ':') {
        for (size_t i = 0; i < n; i++) {
            if (i == 2 || i == 5) continue;
            char c = s[i];
            if (!((c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return -1;
        }
        if (sscanf(s, "%2x:%2x:%2x", &v[0], &v[1], &v[2]) != 3) return -1;
    } else if (n == 6) {
        for (size_t i = 0; i < n; i++) {
            char c = s[i];
            if (!((c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return -1;
        }
        if (sscanf(s, "%2x%2x%2x", &v[0], &v[1], &v[2]) != 3) return -1;
    } else {
        return -1;
    }
    for (int i = 0; i < 3; i++) {
        if (v[i] < 0 || v[i] > 0xFF) return -1;
        oui[i] = (uint8_t)v[i];
    }
    return 0;
}

static int cmd_filter(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("filter", "missing_arg"); return -1; }
    if (!strcmp(argv[1], "any")) {
        s_state.have_mac_filter = false;
        tw32_cli_ack_ok("filter"); return 0;
    }
    uint8_t m[6];
    if (parse_mac(argv[1], m) != 0) {
        tw32_cli_ack_err("filter", "bad_mac"); return -1;
    }
    /* Disable the filter while we update the bytes so the RX path
     * (which reads have_mac_filter then filter_mac without a lock)
     * cannot observe a stale-prefix + new-suffix MAC mid-write. */
    s_state.have_mac_filter = false;
    memcpy(s_state.filter_mac, m, 6);
    s_state.have_mac_filter = true;
    tw32_cli_ack_ok("filter");
    return 0;
}

static int cmd_oui(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("oui", "missing_arg"); return -1; }
    if (!strcmp(argv[1], "any")) {
        s_state.have_oui_filter = false;
        tw32_cli_ack_ok("oui"); return 0;
    }
    uint8_t o[3];
    if (parse_oui(argv[1], o) != 0) {
        tw32_cli_ack_err("oui", "bad_oui"); return -1;
    }
    /* Same transient-state guard as cmd_filter: gate-off, write, gate-on. */
    s_state.have_oui_filter = false;
    memcpy(s_state.filter_oui, o, 3);
    s_state.have_oui_filter = true;
    tw32_cli_ack_ok("oui");
    return 0;
}

static int cmd_pcap(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("pcap", "missing_arg"); return -1; }
    if      (!strcmp(argv[1], "on"))  s_state.pcap_mode = true;
    else if (!strcmp(argv[1], "off")) s_state.pcap_mode = false;
    else { tw32_cli_ack_err("pcap", "use_on_or_off"); return -1; }
    tw32_cli_ack_ok("pcap");
    return 0;
}

static int cmd_peers(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc >= 2 && !strcmp(argv[1], "clear")) {
        xSemaphoreTake(s_peer_lock, portMAX_DELAY);
        memset(s_peers, 0, (size_t)s_peer_slots * sizeof(peer_t));
        s_state.peers_used = 0;
        xSemaphoreGive(s_peer_lock);
        tw32_cli_ack_ok("peers");
        return 0;
    }
    /* Snapshot under the lock so the drain task can't update a slot
     * mid-copy. Buffer is heap-backed (PSRAM when present); allocated
     * once in app_main alongside s_peers. */
    xSemaphoreTake(s_peer_lock, portMAX_DELAY);
    memcpy(s_peer_snap, s_peers, (size_t)s_peer_slots * sizeof(peer_t));
    xSemaphoreGive(s_peer_lock);
    int emitted = 0;
    for (int i = 0; i < s_peer_slots; i++) {
        if (!s_peer_snap[i].used) continue;
        char fp_hex[11];
        snprintf(fp_hex, sizeof(fp_hex), "%08" PRIx32, s_peer_snap[i].fp);
        tw32_json_begin();
        tw32_json_kv_str ("event", "peer");
        tw32_json_kv_mac ("src",   s_peer_snap[i].src);
        tw32_json_kv_uint("first_us", s_peer_snap[i].first_us);
        tw32_json_kv_uint("last_us",  s_peer_snap[i].last_us);
        tw32_json_kv_uint("count",    s_peer_snap[i].count);
        tw32_json_kv_int ("last_rssi", s_peer_snap[i].last_rssi);
        tw32_json_kv_str ("fp", fp_hex);
        tw32_json_end();
        emitted++;
    }
    /* Final ack so the host knows the dump is complete. */
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "peers");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_int ("emitted", emitted);
    tw32_json_end();
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("total",      s_state.total);
    tw32_json_kv_uint("directed",   s_state.directed);
    tw32_json_kv_uint("broadcast",  s_state.broadcast);
    tw32_json_kv_uint("dropped",    s_state.dropped);
    tw32_json_kv_uint("hwm",        s_state.hwm);
    tw32_json_kv_uint("peers_used",   s_state.peers_used);
    tw32_json_kv_uint("peer_slots",   (uint32_t)s_peer_slots);
    tw32_json_kv_uint("mac_filtered", s_state.mac_filtered);
    tw32_json_kv_uint("oui_filtered", s_state.oui_filtered);
    tw32_json_kv_int ("channel",      s_state.channel);
    tw32_json_kv_uint("dwell_ms",     s_state.dwell_ms);
    tw32_json_kv_int ("rssi_min",     s_state.rssi_min);
    tw32_json_kv_bool("hopping",      s_state.hopping);
    tw32_json_kv_bool("pcap",         s_state.pcap_mode);
    tw32_json_kv_bool("filtered_mac", s_state.have_mac_filter);
    if (s_state.have_mac_filter) tw32_json_kv_mac("filter", s_state.filter_mac);
    tw32_json_kv_bool("filtered_oui", s_state.have_oui_filter);
    if (s_state.have_oui_filter) {
        char oui[9];
        snprintf(oui, sizeof(oui), "%02x:%02x:%02x",
                 s_state.filter_oui[0], s_state.filter_oui[1],
                 s_state.filter_oui[2]);
        tw32_json_kv_str("oui", oui);
    }
    tw32_json_kv_bool("running",      s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start",    "begin logging",                          cmd_start    },
    { "stop",     "halt logging",                           cmd_stop     },
    { "chan",     "chan N (1..13)",                         cmd_chan     },
    { "hop",      "hop on|off (channel hopping toggle)",    cmd_hop      },
    { "dwell",    "dwell N (ms per channel; 50..5000)",     cmd_dwell    },
    { "rssi_min", "rssi_min N (accept rssi >= N; -127..0)", cmd_rssi_min },
    { "filter",   "filter <mac|any> (per source MAC)",      cmd_filter   },
    { "oui",      "oui <prefix|any>  (24-bit vendor id)",   cmd_oui      },
    { "pcap",     "pcap on|off (output mode)",              cmd_pcap     },
    { "peers",    "peers | peers clear",                    cmd_peers    },
    { "stats",    "extended counters + state",              cmd_stats    },
};

/* --- main ---------------------------------------------------------------- */

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();

    s_q = xQueueCreate(QUEUE_LEN, sizeof(sniff_rec_t));
    configASSERT(s_q != NULL);
    s_peer_lock = xSemaphoreCreateMutex();
    configASSERT(s_peer_lock != NULL);

    /* Peer table in PSRAM when present, else a smaller internal table. */
    s_peer_slots = PEER_SLOTS_PSRAM;
    s_peers = heap_caps_calloc(s_peer_slots, sizeof(peer_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_peers == NULL) {
        s_peer_slots = PEER_SLOTS_FALLBACK;
        s_peers = calloc(s_peer_slots, sizeof(peer_t));
    }
    configASSERT(s_peers != NULL);
    /* Snapshot buffer: same sizing strategy as the live table. */
    s_peer_snap = heap_caps_calloc(s_peer_slots, sizeof(peer_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_peer_snap == NULL) {
        s_peer_snap = calloc(s_peer_slots, sizeof(peer_t));
    }
    configASSERT(s_peer_snap != NULL);

    /* Persisted defaults. */
    uint32_t ch = tw32_nvs_get_u32("probe", "channel", 0);
    if (ch >= 1 && ch <= 13) s_state.channel = (uint8_t)ch;
    s_state.req_channel = s_state.channel;   /* keep the hopper in sync */
    s_state.hopping  = tw32_nvs_get_bool("probe", "hop", true);
    uint32_t dw = tw32_nvs_get_u32("probe", "dwell_ms", 0);
    if (dw >= 50 && dw <= 5000) s_state.dwell_ms = dw;
    int32_t rmin = tw32_nvs_get_i32("probe", "rssi_min", 0);
    if (rmin >= -127 && rmin <= 0) s_state.rssi_min = (int8_t)rmin;

    wifi_init_promisc();

    xTaskCreatePinnedToCore(drain_task,  "tw32-probe-drain", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(hopper_task, "tw32-probe-hop",   2048, NULL, 3, NULL, 1);

    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
