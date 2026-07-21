/*
 * TheWave32 / ble-airtag-finder
 *
 * Specialised BLE observer for Apple "Find My" / "Offline Finding"
 * advertisements: the protocol AirTags and other Find My-enabled
 * Apple devices use to broadcast their presence to be located by
 * other Apple devices nearby.
 *
 * The signature is an Apple Continuity manufacturer-specific frame
 * with subtype 0x12 (Find My), length 0x19 (25 bytes), i.e. the
 * manufacturer data starts with the byte sequence:
 *
 *     4C 00 12 19 ...
 *
 * The 25-byte payload that follows contains:
 *   1 B  status (battery hints + lost-mode bit)
 *  22 B  rotating advertised public key (rotates ~every 15 minutes)
 *   1 B  bits 6-7 of the public key (extension)
 *   1 B  hint (UI bits)
 *
 * AirTag MACs are themselves rotating (the random address is derived
 * from the public key), so we dedup on a small fingerprint of the
 * first 8 bytes of the public key. Peers stay distinct even as
 * their MAC churns.
 *
 * References:
 *   Heinrich, Stute, Kornhuber, Hollick. "Who Can Find My Devices?
 *     Security and Privacy of Apple's Crowd-Sourced Bluetooth Location
 *     Tracking System." PETS 2021.
 *   seemoo-lab/openhaystack (Find My BLE frame layout).
 *   furiousMAC/continuity (Apple Continuity 0x12 reverse engineering).
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

#define MODULE_NAME    "ble-airtag-finder"
#define MODULE_VERSION "0.3.0"

#define APPLE_MS_OUI_HI 0x00
#define APPLE_MS_OUI_LO 0x4C
#define FINDMY_SUBTYPE  0x12
#define FINDMY_LEN      0x19
#define KEY_HASH_BYTES  8
/* Peer table grows into PSRAM when present; a dense area churns the LRU
 * fast as AirTag keys rotate (~15 min), so more slots keep peers distinct. */
#define PEER_SLOTS_PSRAM    256
#define PEER_SLOTS_FALLBACK  64

typedef struct {
    bool     used;
    uint8_t  key_hash[KEY_HASH_BYTES];
    uint8_t  last_mac[6];
    int8_t   last_rssi;
    uint32_t first_ms;
    uint32_t last_ms;
    uint32_t hits;
} peer_t;

typedef struct {
    volatile bool     running;
    /* adv_seen/airtags_seen written in the GAP callback; distinct_keys
     * under s_peer_lock in two contexts. All read unlocked by the CLI:
     * atomic so the cross-context access is well defined. */
    _Atomic uint32_t  adv_seen;
    _Atomic uint32_t  airtags_seen;     /* every matching adv */
    _Atomic uint32_t  distinct_keys;
} state_t;

static state_t s_state = { 0 };

/* Peer table is shared between the NimBLE GAP callback (writer) and the
 * CLI `peers` command (reader/clear). It was previously touched from both
 * with no lock; this mutex closes that race. Heap-backed (PSRAM when
 * present). */
static peer_t          *s_peers;
static int              s_peer_slots;
static SemaphoreHandle_t s_peer_lock;

static struct ble_gap_disc_params s_disc_params = {
    .itvl = 0x00A0,
    .window = 0x00A0,
    .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
    .limited = 0,
    .passive = 1,
    .filter_duplicates = 0,
};

/* --- AD walker. Find type 0xFF (manufacturer specific). The data we want
 * starts with 4C 00 12 19. Returns pointer to the public key inside the
 * adv buffer, or NULL. Sets *status_out if found.
 *
 * Each AD structure is: [len][type][len-1 bytes of body]. The on-wire
 * `len` byte counts the type byte but not itself, hence the body length
 * is `fl - 1` and the next AD starts at `off + 1 + fl`.
 *
 * All arithmetic stays within `int` range because `len <= 255` (NimBLE
 * passes uint8_t length_data, BLE legacy adv is <= 31 bytes anyway) and
 * both `off` and `fl` are uint8-sized.
 */
static const uint8_t *find_findmy_payload(const uint8_t *data, uint8_t len,
                                          uint8_t *status_out)
{
    unsigned off = 0;
    while (off + 2u <= (unsigned)len) {
        uint8_t fl = data[off];
        uint8_t ft = data[off + 1];
        /* fl must cover its own type byte and at least one body byte for a
         * meaningful structure; off + 1 + fl must not exceed len so the
         * body read below is in-bounds. */
        if (fl == 0 || off + 1u + (unsigned)fl > (unsigned)len) break;
        /* Manufacturer-specific: 1 type byte + at least 4 bytes
         * (OUI lo/hi + Continuity subtype + Continuity length). To then
         * safely read body[4..4+FINDMY_LEN-1] we require fl - 1 >= 4 + 25,
         * i.e. fl >= 30. Check the full length BEFORE touching body[0..3]
         * so a truncated frame never causes an out-of-bounds read. */
        if (ft == 0xFF && fl >= (uint8_t)(1u + 4u + FINDMY_LEN)) {
            const uint8_t *body = data + off + 2;
            if (body[0] == APPLE_MS_OUI_LO && body[1] == APPLE_MS_OUI_HI &&
                body[2] == FINDMY_SUBTYPE && body[3] == FINDMY_LEN) {
                *status_out = body[4];
                return &body[5];                        /* 22 bytes of public key */
            }
        }
        off += 1u + (unsigned)fl;
    }
    return NULL;
}

static int find_or_alloc_peer(const uint8_t key_hash[KEY_HASH_BYTES], uint32_t now_ms,
                              const uint8_t mac[6], int8_t rssi, bool *is_new)
{
    *is_new = false;
    for (int i = 0; i < s_peer_slots; i++) {
        if (s_peers[i].used &&
            memcmp(s_peers[i].key_hash, key_hash, KEY_HASH_BYTES) == 0) {
            s_peers[i].last_ms   = now_ms;
            s_peers[i].last_rssi = rssi;
            s_peers[i].hits++;
            memcpy(s_peers[i].last_mac, mac, 6);
            return i;
        }
    }
    /* Free slot or LRU eviction. */
    int slot = -1;
    uint32_t oldest_ms = UINT32_MAX;
    for (int i = 0; i < s_peer_slots; i++) {
        if (!s_peers[i].used) { slot = i; break; }
        if (s_peers[i].last_ms < oldest_ms) {
            oldest_ms = s_peers[i].last_ms;
            slot = i;
        }
    }
    if (slot < 0) return -1;
    memset(&s_peers[slot], 0, sizeof(peer_t));
    s_peers[slot].used = true;
    memcpy(s_peers[slot].key_hash, key_hash, KEY_HASH_BYTES);
    memcpy(s_peers[slot].last_mac, mac, 6);
    s_peers[slot].last_rssi = rssi;
    s_peers[slot].first_ms  = now_ms;
    s_peers[slot].last_ms   = now_ms;
    s_peers[slot].hits      = 1;
    s_state.distinct_keys++;
    *is_new = true;
    return slot;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    /* ble_gap_disc_cancel() is racy with in-flight host events; drop any
     * report queued before the cancel was processed so counters don't
     * keep ticking after `stop`. */
    if (!s_state.running) return 0;
    const struct ble_gap_disc_desc *d = &event->disc;
    s_state.adv_seen++;

    if (d->length_data == 0 || d->data == NULL) return 0;

    uint8_t status = 0;
    const uint8_t *pubkey = find_findmy_payload(d->data, d->length_data, &status);
    if (!pubkey) return 0;

    s_state.airtags_seen++;

    uint8_t key_hash[KEY_HASH_BYTES];
    memcpy(key_hash, pubkey, KEY_HASH_BYTES);

    /* MAC arrives little-endian; emit big-endian. */
    uint8_t mac_be[6];
    for (int i = 0; i < 6; i++) mac_be[i] = d->addr.val[5 - i];

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool is_new = false;
    /* Serialise table access against cmd_peers (CLI). The key_hash/is_new
     * we use for the emit below are local, so the lock only spans the
     * table update, not the USB write. */
    xSemaphoreTake(s_peer_lock, portMAX_DELAY);
    find_or_alloc_peer(key_hash, now_ms, mac_be, d->rssi, &is_new);
    xSemaphoreGive(s_peer_lock);

    /* Emit hex of key_hash. */
    char hex[2 * KEY_HASH_BYTES + 1];
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < KEY_HASH_BYTES; i++) {
        hex[2 * i]     = h[key_hash[i] >> 4];
        hex[2 * i + 1] = h[key_hash[i] & 0x0f];
    }
    hex[2 * KEY_HASH_BYTES] = '\0';

    tw32_json_begin();
    tw32_json_kv_str ("event", "airtag");
    tw32_json_kv_uint("ts",    now_ms);
    tw32_json_kv_mac ("mac",   mac_be);
    tw32_json_kv_int ("rssi",  d->rssi);
    tw32_json_kv_uint("status", status);
    tw32_json_kv_str ("key_hash", hex);
    tw32_json_kv_bool("new",    is_new);
    tw32_json_end();
    return 0;
}

static void on_sync(void) { /* host ready */ }
static void on_reset(int r) { (void)r; s_state.running = false; }

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

/* --- CLI ----------------------------------------------------------- */

static int cmd_start(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    if (s_state.running) { tw32_cli_ack_ok("start"); return 0; }
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &s_disc_params,
                          gap_event_cb, NULL);
    if (rc != 0) {
        char err[24];
        snprintf(err, sizeof(err), "ble_gap_disc=%d", rc);
        tw32_cli_ack_err("start", err);
        return -1;
    }
    s_state.running = true;
    tw32_cli_ack_ok("start");
    return 0;
}

static int cmd_stop(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    if (s_state.running) { ble_gap_disc_cancel(); s_state.running = false; }
    tw32_cli_ack_ok("stop");
    return 0;
}

static int cmd_peers(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc >= 2 && !strcmp(argv[1], "clear")) {
        xSemaphoreTake(s_peer_lock, portMAX_DELAY);
        memset(s_peers, 0, (size_t)s_peer_slots * sizeof(peer_t));
        s_state.distinct_keys = 0;
        xSemaphoreGive(s_peer_lock);
        tw32_cli_ack_ok("peers");
        return 0;
    }
    /* Snapshot under the lock so the GAP callback can't mutate a slot
     * mid-emit; buffer sized for the largest possible table. The build
     * fails loudly if anyone bumps PEER_SLOTS_PSRAM past the snapshot
     * size without resizing this buffer. */
    static peer_t snap[PEER_SLOTS_PSRAM];
    _Static_assert(PEER_SLOTS_PSRAM >= PEER_SLOTS_FALLBACK,
                   "snapshot must fit any runtime peer table size");
    xSemaphoreTake(s_peer_lock, portMAX_DELAY);
    int slots = s_peer_slots;
    if (slots > (int)(sizeof(snap) / sizeof(snap[0]))) {
        slots = (int)(sizeof(snap) / sizeof(snap[0]));
    }
    memcpy(snap, s_peers, (size_t)slots * sizeof(peer_t));
    xSemaphoreGive(s_peer_lock);
    int emitted = 0;
    for (int i = 0; i < slots; i++) {
        if (!snap[i].used) continue;
        emitted++;
        char hex[2 * KEY_HASH_BYTES + 1];
        static const char *h = "0123456789abcdef";
        for (int k = 0; k < KEY_HASH_BYTES; k++) {
            hex[2*k]   = h[snap[i].key_hash[k] >> 4];
            hex[2*k+1] = h[snap[i].key_hash[k] & 0x0f];
        }
        hex[2 * KEY_HASH_BYTES] = '\0';
        tw32_json_begin();
        tw32_json_kv_str ("event",     "peer");
        tw32_json_kv_str ("key_hash",  hex);
        tw32_json_kv_mac ("last_mac",  snap[i].last_mac);
        tw32_json_kv_int ("last_rssi", snap[i].last_rssi);
        tw32_json_kv_uint("first_ms",  snap[i].first_ms);
        tw32_json_kv_uint("last_ms",   snap[i].last_ms);
        tw32_json_kv_uint("hits",      snap[i].hits);
        tw32_json_end();
    }
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "peers");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_int ("count", emitted);
    tw32_json_end();
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_uint("adv_seen",       s_state.adv_seen);
    tw32_json_kv_uint("airtags_seen",   s_state.airtags_seen);
    tw32_json_kv_uint("distinct_keys",  s_state.distinct_keys);
    tw32_json_kv_uint("peer_slots",     (uint32_t)s_peer_slots);
    tw32_json_kv_bool("running",        s_state.running);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start", "begin Find My scan",          cmd_start },
    { "stop",  "halt scan",                   cmd_stop  },
    { "peers", "peers | peers clear",         cmd_peers },
    { "stats", "counters + state",            cmd_stats },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();

    s_peer_lock = xSemaphoreCreateMutex();
    configASSERT(s_peer_lock != NULL);
    s_peer_slots = PEER_SLOTS_PSRAM;
    s_peers = heap_caps_calloc(s_peer_slots, sizeof(peer_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_peers == NULL) {
        s_peer_slots = PEER_SLOTS_FALLBACK;
        s_peers = calloc(s_peer_slots, sizeof(peer_t));
    }
    configASSERT(s_peers != NULL);

    ble_init();
    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
