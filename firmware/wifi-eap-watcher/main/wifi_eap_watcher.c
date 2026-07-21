/*
 * TheWave32 / wifi-eap-watcher
 *
 * Passive 802.1X EAP-Identity-Response observer for Wi-Fi.
 *
 * What it does
 * ------------
 * Sets the radio to promiscuous mode, walks 802.11 data frames, and
 * pulls the EAP-Identity-Response value from EAPOL exchanges that
 * cross the air *before* WPA-Enterprise key derivation has happened
 * (i.e. while the EAPOL traffic is still in cleartext at the MAC
 * layer). It then emits one JSON line per Identity-Response observed:
 *
 *   {"event":"eap_id","ts":...,"src":"aa:bb:cc:dd:ee:ff",
 *    "bssid":"...","ch":N,"rssi":-65,
 *    "eap_id":"1<imsi>@wlan.mnc<mnc>.mcc<mcc>.3gppnetwork.org",
 *    "eap_method":"sim","imsi":"<digits>"}
 *
 * Why this exists
 * ---------------
 * EAP-SIM (RFC 4186), EAP-AKA (RFC 4187), and EAP-AKA' (RFC 5448)
 * historically allow a station to identify itself with a permanent
 * IMSI-derived NAI in the EAP-Identity-Response. This exchange is
 * MAC-layer plaintext until the 4-way handshake completes. The RFCs
 * say implementations SHOULD use pseudonyms or fast-reauth identities
 * to avoid leaking the IMSI; misconfigured carrier hotspots and
 * legacy EAP-SIM only deployments still fall back to the bare IMSI.
 *
 * Intended use
 * ------------
 *   - Auditing your own WPA-Enterprise / Hotspot 2.0 / Wi-Fi Calling
 *     deployment to confirm pseudonyms are being used.
 *   - Privacy research with bench-test devices on networks you own.
 *   - Demonstrating the legacy EAP-SIM weakness in security training.
 *
 * What it is NOT
 * --------------
 *   - An IMSI catcher in the Stingray sense. There is no transmission
 *     here. The radio TX path is never enabled. We do not impersonate
 *     a base station, we do not deauth, we do not coerce stations.
 *   - A general EAPOL/EAP capture tool. We narrow the parser to
 *     EAP-Identity Responses only, and we only report the identity.
 *     Other EAP frames (challenges, MAC values, etc.) are skipped.
 *
 * Operating outside one of those contexts (capturing IMSI-bearing
 * EAP-Identity exchanges from third parties on networks you don't
 * own/operate) is wiretapping in most jurisdictions. The operator —
 * not the firmware — is responsible for that line.
 */

#include <ctype.h>
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

#define MODULE_NAME    "wifi-eap-watcher"
#define MODULE_VERSION "0.2.0"

/*
 * 802.11 frame-control byte 0:
 *   - Data frames have type=10. Subtypes:
 *       0x08 = vanilla data
 *       0x88 = QoS data (24+2 byte header)
 *       0x40 in upper nibble of byte 0 maps to "null" subtypes that
 *         don't carry payload — skip.
 *   - We accept FC[0] == 0x08 or 0x88. Other data subtypes (CF-Poll,
 *     Null, etc.) carry no body so are uninteresting.
 *
 * FC[1] bit 6 (0x40) is "Protected Frame". Once the 4-way handshake
 * completes, payload is encrypted and we cannot read EAPOL anymore.
 * We skip Protected frames.
 */
#define FC0_DATA            0x08
#define FC0_QOS_DATA        0x88
#define FC1_PROTECTED_BIT   0x40

/* LLC/SNAP header that wraps the EtherType inside an 802.11 data frame.
 *   AA AA 03 00 00 00 ETH_TYPE_HI ETH_TYPE_LO
 */
#define LLC_SNAP_LEN        8
#define ETHERTYPE_EAPOL     0x888E

/* EAPOL header: version (1) | type (1) | length (2). */
#define EAPOL_HDR_LEN       4
#define EAPOL_TYPE_EAP      0x00 /* EAPOL-EAP-Packet (carries an EAP packet) */

/* EAP packet header: code (1) | id (1) | length (2) | type (1) */
#define EAP_HDR_LEN         5
#define EAP_CODE_RESPONSE   2
#define EAP_TYPE_IDENTITY   1

#define IDENTITY_MAX        128
#define IMSI_MAX_DIGITS     15
#define QUEUE_LEN           16

typedef struct {
    uint32_t ts_us;
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  src[6];
    uint8_t  bssid[6];
    uint8_t  id_len;
    char     id[IDENTITY_MAX + 1]; /* NUL-terminated */
} eap_evt_t;

typedef struct {
    volatile bool     running;
    volatile bool     hopping;
    volatile uint8_t  channel;        /* current; written only by hopper_task */
    volatile uint8_t  req_channel;    /* desired channel when not hopping */
    volatile uint32_t dwell_ms;
    /* Counters written on the Wi-Fi RX core (eapol_seen/dropped) or the
     * drain core (identities/imsi_matches) and read by the CLI: atomic so
     * the cross-core RMW/read is defined rather than a data race. */
    _Atomic uint32_t total_data;     /* unencrypted data frames seen */
    _Atomic uint32_t eapol_seen;
    _Atomic uint32_t identities;     /* EAP-Identity-Response decoded */
    _Atomic uint32_t imsi_matches;
    _Atomic uint32_t dropped;
} state_t;

static state_t s_state = {
    .running = false,
    .hopping = true,
    .channel = 1,
    .req_channel = 1,
    .dwell_ms = 250,
};

static QueueHandle_t s_q;

/* --- helpers ------------------------------------------------------------- */

/* Classify an EAP-SIM/AKA/AKA' identity. Return method name (sim/aka/aka')
 * if the prefix matches and the leading digits look like an IMSI; otherwise
 * NULL and *imsi_out empty. */
static const char *classify_identity(const char *id, char *imsi_out, size_t imsi_cap)
{
    imsi_out[0] = '\0';
    if (!id || id[0] == '\0') return NULL;
    char prefix = id[0];
    const char *method = NULL;
    if      (prefix == '1') method = "sim";
    else if (prefix == '0') method = "aka";
    else if (prefix == '6') method = "aka'";
    else return NULL;

    /* Walk digits up to '@' or end. */
    size_t digits = 0;
    size_t i = 1;
    while (id[i] && id[i] != '@' && digits < imsi_cap - 1) {
        if (!isdigit((unsigned char)id[i])) return NULL;
        imsi_out[digits++] = id[i++];
    }
    imsi_out[digits] = '\0';
    /* IMSI is 14 or 15 digits per ITU-T E.212 (some carriers use 14). */
    if (digits < 14 || digits > IMSI_MAX_DIGITS) {
        imsi_out[0] = '\0';
        return NULL;
    }
    return method;
}

static bool is_printable_identity(const uint8_t *p, uint8_t n)
{
    /* RFC 4282/7542 NAI is ASCII-printable. Reject anything weird. */
    for (uint8_t i = 0; i < n; ++i) {
        if (p[i] < 0x20 || p[i] >= 0x7f) return false;
    }
    return true;
}

/* --- promisc cb (RX path) ------------------------------------------------ */

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_state.running) return;
    if (type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *fr = p->payload;
    /* sig_len includes the trailing 4-byte FCS (see ESP-IDF
     * components/esp_wifi/include/local/esp_wifi_types_native.h). Strip
     * it so every bound below operates on real payload bytes, and a
     * forged EAP length cannot make us treat FCS as Identity bytes. */
    uint16_t flen = p->rx_ctrl.sig_len;
    if (flen < 4u) return;
    uint16_t payload_len = flen - 4u;

    if (payload_len < 24 + LLC_SNAP_LEN + EAPOL_HDR_LEN + EAP_HDR_LEN) return;
    /* Only data subtypes that carry a payload. */
    if (fr[0] != FC0_DATA && fr[0] != FC0_QOS_DATA) return;
    /* Skip encrypted frames: payload is unreadable. */
    if (fr[1] & FC1_PROTECTED_BIT) return;
    /* EAP-Identity-Response travels from the supplicant to the AP, so the
     * DS bits MUST be To-DS=1, From-DS=0. Enforcing this guarantees that
     * Address1=BSSID, Address2=SA, Address3=DA (the layout trusted at
     * fr+4 / fr+10 below). Any other direction has SA in a different
     * slot and would publish the wrong source MAC. */
    if ((fr[1] & 0x03) != 0x01) return;

    /* QoS data has 2 extra bytes after the standard 24-byte header. */
    uint16_t hdrlen = (fr[0] == FC0_QOS_DATA) ? 26 : 24;
    if (payload_len < hdrlen + LLC_SNAP_LEN + EAPOL_HDR_LEN + EAP_HDR_LEN) return;

    const uint8_t *llc = fr + hdrlen;
    /* LLC/SNAP: AA AA 03 00 00 00 + 2-byte EtherType. */
    if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03 ||
        llc[3] != 0x00 || llc[4] != 0x00 || llc[5] != 0x00) {
        return;
    }
    uint16_t ethertype = ((uint16_t)llc[6] << 8) | llc[7];
    if (ethertype != ETHERTYPE_EAPOL) return;

    s_state.eapol_seen++;

    const uint8_t *eapol = llc + LLC_SNAP_LEN;
    /* EAPOL: version | type | length(2) | body. IEEE 802.1X-2010 defines
     * protocol versions 1, 2 and 3; anything else is a malformed frame
     * (or a non-EAPOL payload that happened to start with the EAPOL
     * EtherType after corruption) and the body must not be trusted. */
    uint8_t eapol_ver  = eapol[0];
    uint8_t eapol_type = eapol[1];
    if (eapol_ver == 0 || eapol_ver > 3) return;
    if (eapol_type != EAPOL_TYPE_EAP) return;

    uint16_t eapol_body_len = ((uint16_t)eapol[2] << 8) | eapol[3];
    if (eapol_body_len < EAP_HDR_LEN) return;
    /* Bound against real payload (FCS already excluded above). */
    if ((uint32_t)hdrlen + LLC_SNAP_LEN + EAPOL_HDR_LEN + eapol_body_len > payload_len) return;

    const uint8_t *eap = eapol + EAPOL_HDR_LEN;
    uint8_t eap_code   = eap[0];
    uint16_t eap_len   = ((uint16_t)eap[2] << 8) | eap[3];
    uint8_t  eap_type  = eap[4];

    if (eap_code != EAP_CODE_RESPONSE) return;
    if (eap_type != EAP_TYPE_IDENTITY) return;
    if (eap_len < EAP_HDR_LEN || eap_len > eapol_body_len) return;

    /* Identity bytes follow the EAP header (5 bytes in). */
    uint16_t id_len = eap_len - EAP_HDR_LEN;
    if (id_len > IDENTITY_MAX) id_len = IDENTITY_MAX;
    const uint8_t *id_bytes = eap + EAP_HDR_LEN;

    if (!is_printable_identity(id_bytes, id_len)) return;

    eap_evt_t e;
    e.ts_us   = (uint32_t)esp_timer_get_time();
    e.rssi    = p->rx_ctrl.rssi;
    e.channel = p->rx_ctrl.channel;
    /*
     * 802.11 data MAC header field semantics depend on the To-DS / From-DS
     * bits in FC[1] (bits 0..1). For station-to-AP frames (To-DS=1,
     * From-DS=0): Address1=BSSID, Address2=SA, Address3=DA. The
     * EAP-Identity-Response always travels in this direction (station
     * answering the AP), so SA = bytes 10..15 and BSSID = bytes 4..9.
     */
    memcpy(e.bssid, fr + 4,  6);
    memcpy(e.src,   fr + 10, 6);
    memcpy(e.id,    id_bytes, id_len);
    e.id[id_len] = '\0';
    e.id_len = (uint8_t)id_len;

    if (xQueueSend(s_q, &e, 0) != pdTRUE) {
        s_state.dropped++;
    }
}

/* --- drainer + hopper ---------------------------------------------------- */

static void emit(const eap_evt_t *e)
{
    char imsi[IMSI_MAX_DIGITS + 1];
    const char *method = classify_identity(e->id, imsi, sizeof(imsi));

    s_state.identities++;
    if (method) s_state.imsi_matches++;

    tw32_json_begin();
    tw32_json_kv_str ("event", "eap_id");
    tw32_json_kv_uint("ts",    e->ts_us);
    tw32_json_kv_mac ("src",   e->src);
    tw32_json_kv_mac ("bssid", e->bssid);
    tw32_json_kv_int ("ch",    e->channel);
    tw32_json_kv_int ("rssi",  e->rssi);
    tw32_json_kv_str ("eap_id", e->id);
    if (method) {
        tw32_json_kv_str("eap_method", method);
        tw32_json_kv_str("imsi",        imsi);
    } else {
        tw32_json_kv_str("eap_method", "non_imsi");
    }
    tw32_json_end();
}

static void drain_task(void *arg)
{
    (void)arg;
    eap_evt_t e;
    while (true) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) == pdTRUE) {
            emit(&e);
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
                /* Apply a CLI pin request. The hopper is the single writer
                 * of `channel`, so cmd_chan can never race it. */
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

/* --- bring-up ------------------------------------------------------------ */

static void wifi_init_promisc(void)
{
    { esp_err_t _e = esp_event_loop_create_default();
      if (_e != ESP_OK && _e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(_e); }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Want data frames; mgmt frames irrelevant for EAPOL. */
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&promisc_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_state.channel, WIFI_SECOND_CHAN_NONE));
}

/* --- CLI ----------------------------------------------------------------- */

static int cmd_start(tw32_cli_ctx_t *ctx, int argc, char **argv)
{ (void)ctx;(void)argc;(void)argv; s_state.running = true;  tw32_cli_ack_ok("start"); return 0; }

static int cmd_stop(tw32_cli_ctx_t *ctx, int argc, char **argv)
{ (void)ctx;(void)argc;(void)argv; s_state.running = false; tw32_cli_ack_ok("stop");  return 0; }

static int cmd_chan(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("chan", "missing_arg"); return -1; }
    long ch = strtol(argv[1], NULL, 10);
    if (ch < 1 || ch > 14) { tw32_cli_ack_err("chan", "out_of_range"); return -1; }
    /* Record the request; the hopper (single writer of the channel) applies
     * it within ~50 ms. Pinning implicitly disables hopping. */
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

static int cmd_inject_test(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    /* For self-test only: hand-craft an EAP-Identity-Response payload
     * and feed it through the same emit path so the operator can verify
     * the parser without a live WPA-Enterprise client around. The
     * argument is the identity string (defaults to a synthetic IMSI). */
    (void)ctx;
    eap_evt_t e = {0};
    const char *id = (argc >= 2) ? argv[1] : "1724080123456789@wlan.mnc080.mcc724.3gppnetwork.org";
    size_t L = strlen(id);
    if (L > IDENTITY_MAX) L = IDENTITY_MAX;
    memcpy(e.id, id, L); e.id[L] = '\0'; e.id_len = (uint8_t)L;
    e.ts_us = (uint32_t)esp_timer_get_time();
    e.rssi  = 0;
    e.channel = s_state.channel;
    /* Synthetic addresses so the parser path looks normal. */
    static const uint8_t fake_src[6]   = {0x02,0x00,0x00,0x00,0x00,0x01};
    static const uint8_t fake_bssid[6] = {0x02,0x00,0x00,0x00,0x00,0x02};
    memcpy(e.src,   fake_src,   6);
    memcpy(e.bssid, fake_bssid, 6);
    if (xQueueSend(s_q, &e, 0) == pdTRUE) {
        tw32_cli_ack_ok("inject_test");
    } else {
        tw32_cli_ack_err("inject_test", "queue_full");
    }
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("eapol_seen",   s_state.eapol_seen);
    tw32_json_kv_uint("identities",   s_state.identities);
    tw32_json_kv_uint("imsi_matches", s_state.imsi_matches);
    tw32_json_kv_uint("dropped",      s_state.dropped);
    tw32_json_kv_int ("channel",      s_state.channel);
    tw32_json_kv_uint("dwell_ms",     s_state.dwell_ms);
    tw32_json_kv_bool("hopping",      s_state.hopping);
    tw32_json_kv_bool("running",      s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start",       "begin watching",                       cmd_start       },
    { "stop",        "halt watching",                        cmd_stop        },
    { "chan",        "chan N (1..13)",                       cmd_chan        },
    { "hop",         "hop on|off",                           cmd_hop         },
    { "dwell",       "dwell N (ms per channel; 50..5000)",   cmd_dwell       },
    { "inject_test", "inject_test [<identity>] — feed the\n"
                     "              parser a synthetic EAP-Id event",
                                                             cmd_inject_test },
    { "stats",       "counters + state",                     cmd_stats       },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();

    s_q = xQueueCreate(QUEUE_LEN, sizeof(eap_evt_t));
    configASSERT(s_q != NULL);

    uint32_t ch = tw32_nvs_get_u32("eap", "channel", 0);
    if (ch >= 1 && ch <= 13) s_state.channel = (uint8_t)ch;
    s_state.req_channel = s_state.channel;   /* keep the hopper in sync */
    s_state.hopping = tw32_nvs_get_bool("eap", "hop", true);
    uint32_t dw = tw32_nvs_get_u32("eap", "dwell_ms", 0);
    if (dw >= 50 && dw <= 5000) s_state.dwell_ms = dw;

    wifi_init_promisc();

    xTaskCreatePinnedToCore(drain_task,  "tw32-eap-drain", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(hopper_task, "tw32-eap-hop",   2048, NULL, 3, NULL, 1);

    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
