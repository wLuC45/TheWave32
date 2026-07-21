/*
 * TheWave32 / ble-hid-keyboard
 *
 * BLE HID keyboard with a tiny DuckyScript interpreter.
 *
 * On boot, registers a HID-over-GATT keyboard service (boot mode +
 * report mode), advertises with the device name "TW32-Keyboard" and
 * the HID appearance (961 / Keyboard), and waits for a host to bond.
 * Once subscribed, after `start_delay_ms`, it walks the DuckyScript
 * payload from NVS (`ducky/payload`) and emits keystrokes via the
 * Report characteristic.
 *
 * DuckyScript subset:
 *   DEFAULTDELAY <ms>     — between commands
 *   DELAY <ms>            — once
 *   STRING <text>         — type literal text (ASCII printable only)
 *   ENTER, TAB, SPACE, BACKSPACE, ESC, DELETE
 *   GUI / WINDOWS / SUPER, CTRL, ALT, SHIFT — modifier with optional
 *     trailing key letter (e.g. "GUI r", "CTRL ALT DEL")
 *   F1..F12
 *   COMMENTS  — lines starting with REM are ignored
 *
 * The runtime CLI on UART0 is mostly observability; the script kicks
 * off automatically once a host subscribes to the input report. CLI:
 *   start_delay <ms>      — set delay (also persisted to NVS)
 *   payload set <text>    — replace payload in NVS
 *   payload get           — print current payload (truncated to 256 B in JSON)
 *   replay                — re-run the payload (requires active connection)
 *   stats                 — connection state + counters
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"

#define MODULE_NAME    "ble-hid-keyboard"
#define MODULE_VERSION "0.2.0"

#define DEVICE_NAME    "TW32-Keyboard"
#define HID_APPEARANCE 0x03C1   /* Keyboard */

#define UUID16_HID            0x1812
#define UUID16_HID_INFORMATION 0x2A4A
#define UUID16_REPORT_MAP      0x2A4B
#define UUID16_HID_CTL_POINT   0x2A4C
#define UUID16_REPORT          0x2A4D
#define UUID16_PROTO_MODE      0x2A4E
#define UUID16_BATT_SERVICE    0x180F
#define UUID16_BATT_LEVEL      0x2A19
#define UUID16_DEV_INFO        0x180A

/* Standard HID report descriptor: boot keyboard, 8-byte input report
 * (modifiers, reserved, 6 keycodes), 1-byte output (LEDs). Source:
 * USB HID 1.11 spec Appendix B.1, used verbatim by virtually every
 * BLE keyboard. */
static const uint8_t s_hid_report_map[] = {
    0x05, 0x01,  /* Usage Page (Generic Desktop) */
    0x09, 0x06,  /* Usage (Keyboard) */
    0xA1, 0x01,  /*   Collection (Application) */
    0x05, 0x07,  /*     Usage Page (Key Codes) */
    0x19, 0xE0,  /*     Usage Min (224) */
    0x29, 0xE7,  /*     Usage Max (231) */
    0x15, 0x00,  /*     Logical Min (0) */
    0x25, 0x01,  /*     Logical Max (1) */
    0x75, 0x01,  /*     Report Size (1) */
    0x95, 0x08,  /*     Report Count (8)   — modifier byte */
    0x81, 0x02,  /*     Input (Data,Var,Abs) */
    0x95, 0x01,  /*     Report Count (1)   — reserved */
    0x75, 0x08,  /*     Report Size (8) */
    0x81, 0x01,  /*     Input (Const) */
    0x95, 0x05,  /*     Report Count (5)   — LED bits */
    0x75, 0x01,  /*     Report Size (1) */
    0x05, 0x08,  /*     Usage Page (LEDs) */
    0x19, 0x01,  /*     Usage Min (1) */
    0x29, 0x05,  /*     Usage Max (5) */
    0x91, 0x02,  /*     Output (Data,Var,Abs) */
    0x95, 0x01,  /*     Report Count (1)   — LED padding */
    0x75, 0x03,  /*     Report Size (3) */
    0x91, 0x01,  /*     Output (Const) */
    0x95, 0x06,  /*     Report Count (6)   — keys */
    0x75, 0x08,  /*     Report Size (8) */
    0x15, 0x00,  /*     Logical Min (0) */
    0x25, 0x65,  /*     Logical Max (101) */
    0x05, 0x07,  /*     Usage Page (Key Codes) */
    0x19, 0x00,  /*     Usage Min (0) */
    0x29, 0x65,  /*     Usage Max (101) */
    0x81, 0x00,  /*     Input (Data,Array) */
    0xC0,        /*   End Collection */
};

/* HID Information value (BCD ver=0x0111, country=0, flags=0x02
 * normally connectable). */
static const uint8_t s_hid_info[] = { 0x11, 0x01, 0x00, 0x02 };

/* Modifier byte bits (HID 1.11 §8.3). */
#define MOD_CTRL_L  (1 << 0)
#define MOD_SHIFT_L (1 << 1)
#define MOD_ALT_L   (1 << 2)
#define MOD_GUI_L   (1 << 3)

/* Selected HID usage IDs (HID Usage Tables §10). */
#define KC_ENTER     0x28
#define KC_ESC       0x29
#define KC_BACKSPACE 0x2A
#define KC_TAB       0x2B
#define KC_SPACE     0x2C
#define KC_DELETE    0x4C
#define KC_F1        0x3A
#define KC_R         0x15

typedef struct {
    volatile bool connected;
    volatile bool subscribed;
    volatile uint16_t conn_handle;
    volatile uint16_t input_val_handle;
    /* Incremented by send_report(), which runs from both the auto-run
     * worker task and the CLI `type`/`replay` path; read by the CLI in
     * stats. Atomic so the two writers don't lose increments. */
    _Atomic uint32_t  reports_sent;
    volatile uint32_t start_delay_ms;
    volatile bool ran;
} state_t;

static state_t s_state = {
    .start_delay_ms = 2000,
    .conn_handle = BLE_HS_CONN_HANDLE_NONE,
};

/* --- HID GATT report attribute callbacks --------------------------------- */

static int report_map_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, s_hid_report_map, sizeof(s_hid_report_map))
                   == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int hid_info_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, s_hid_info, sizeof(s_hid_info))
                   == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int ctl_point_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* Suspend / Exit-Suspend; we ignore the value. */
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return 0;
}

static int proto_mode_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    static uint8_t mode = 1; /* report mode */
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &mode, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (OS_MBUF_PKTLEN(ctxt->om) >= 1) {
            uint16_t cl;
            ble_hs_mbuf_to_flat(ctxt->om, &mode, 1, &cl);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* Written by NimBLE during GATT registration, read from runner_task. The
 * write happens once at boot before notifies start, so a 16-bit aligned
 * read on Xtensa is safe without further synchronisation. */
static uint16_t s_input_attr_handle;

static int input_report_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)arg;
    static const uint8_t empty[8] = {0};
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, empty, sizeof(empty))
                   == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* Battery Level (always 100). */
static int batt_lvl_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    static const uint8_t lvl = 100;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &lvl, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* Report Reference descriptor (Report ID + type). One per report char. */
static const uint8_t s_input_rep_ref[] = { 0x00, 0x01 };  /* id 0, input */
static int input_rep_ref_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        return os_mbuf_append(ctxt->om, s_input_rep_ref, sizeof(s_input_rep_ref))
                   == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* --- GATT service tree --------------------------------------------------- */

static const ble_uuid16_t uuid_hid_service     = BLE_UUID16_INIT(UUID16_HID);
static const ble_uuid16_t uuid_hid_info        = BLE_UUID16_INIT(UUID16_HID_INFORMATION);
static const ble_uuid16_t uuid_report_map      = BLE_UUID16_INIT(UUID16_REPORT_MAP);
static const ble_uuid16_t uuid_hid_ctl         = BLE_UUID16_INIT(UUID16_HID_CTL_POINT);
static const ble_uuid16_t uuid_report          = BLE_UUID16_INIT(UUID16_REPORT);
static const ble_uuid16_t uuid_proto_mode      = BLE_UUID16_INIT(UUID16_PROTO_MODE);
static const ble_uuid16_t uuid_report_ref      = BLE_UUID16_INIT(0x2908);
static const ble_uuid16_t uuid_batt_service    = BLE_UUID16_INIT(UUID16_BATT_SERVICE);
static const ble_uuid16_t uuid_batt_level      = BLE_UUID16_INIT(UUID16_BATT_LEVEL);

static const struct ble_gatt_dsc_def input_descriptors[] = {
    { .uuid = &uuid_report_ref.u, .att_flags = BLE_ATT_F_READ, .access_cb = input_rep_ref_cb },
    { 0 }
};

static const struct ble_gatt_chr_def hid_chars[] = {
    {
        .uuid = &uuid_hid_info.u,
        .access_cb = hid_info_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
    },
    {
        .uuid = &uuid_report_map.u,
        .access_cb = report_map_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
    },
    {
        .uuid = &uuid_hid_ctl.u,
        .access_cb = ctl_point_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &uuid_proto_mode.u,
        .access_cb = proto_mode_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        /* Input report (notify). Saved attr handle for later notification. */
        .uuid = &uuid_report.u,
        .access_cb = input_report_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
        .descriptors = input_descriptors,
        .val_handle = &s_input_attr_handle,
    },
    { 0 }
};

static const struct ble_gatt_chr_def batt_chars[] = {
    {
        .uuid = &uuid_batt_level.u,
        .access_cb = batt_lvl_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    { 0 }
};

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_hid_service.u,
        .characteristics = hid_chars,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_batt_service.u,
        .characteristics = batt_chars,
    },
    { 0 }
};

/* --- advertising / GAP -------------------------------------------------- */

static void start_advertising(void);

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_state.connected = true;
                s_state.conn_handle = event->connect.conn_handle;
            } else {
                start_advertising();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            s_state.connected = false;
            s_state.subscribed = false;
            s_state.ran = false;
            /* Park conn_handle in the sentinel so a runner snapshot taken
             * between disconnect and the next connect cannot pass a stale
             * 16-bit value into ble_gatts_notify_custom. Aligned 16-bit
             * loads/stores are atomic on Xtensa LX7 (ESP32-S3 TRM, ch. 3
             * memory model: naturally aligned <= word-size accesses are
             * single-cycle, indivisible) so a torn read is not possible,
             * but a stale value still would be without this reset. */
            s_state.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            start_advertising();
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_input_attr_handle) {
                /* HOGP §6: the host writes CCCD to enable/disable
                 * notifications. Track both transitions so a host that
                 * disables notifications mid-session immediately stops
                 * the runner instead of looping until BLE_HS_ENOTCONN. */
                s_state.subscribed = (event->subscribe.cur_notify != 0);
                if (!s_state.subscribed) {
                    s_state.ran = false;
                }
            }
            break;
        case BLE_GAP_EVENT_PASSKEY_ACTION:
            /* Just-Works: no PIN exchange. */
            break;
        default:
            break;
    }
    return 0;
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.appearance = HID_APPEARANCE;
    fields.appearance_is_present = 1;
    static const ble_uuid16_t uuid_list[] = { BLE_UUID16_INIT(UUID16_HID) };
    fields.uuids16 = uuid_list;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        return;
    }
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &params, gap_event_cb, NULL);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    (void)rc;
    start_advertising();
}

static void on_reset(int reason) { (void)reason; }

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* --- HID report sending ------------------------------------------------- */

static void send_report(uint8_t mods, uint8_t kc)
{
    /* ble_gatts_notify_custom is itself thread-safe and validates the
     * conn_handle internally; if the host disconnected between us checking
     * `subscribed` and the call dispatching, the call returns BLE_HS_ENOTCONN
     * and we abort the script. Snapshot the handles into locals first so we
     * pass a single coherent value to both notifies. */
    if (!s_state.subscribed) return;
    uint16_t ch   = s_state.conn_handle;
    uint16_t attr = s_input_attr_handle;
    if (ch == BLE_HS_CONN_HANDLE_NONE) {
        /* subscribed just flipped true but disconnect raced in. Abort. */
        s_state.subscribed = false;
        return;
    }

    uint8_t r[8] = { mods, 0, kc, 0, 0, 0, 0, 0 };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(r, sizeof(r));
    if (om != NULL) {
        int rc = ble_gatts_notify_custom(ch, attr, om);
        if (rc != 0) {
            s_state.subscribed = false;
            return;
        }
        s_state.reports_sent++;
    }
    /* Key release report. */
    memset(r, 0, sizeof(r));
    om = ble_hs_mbuf_from_flat(r, sizeof(r));
    if (om != NULL) {
        int rc = ble_gatts_notify_custom(ch, attr, om);
        if (rc != 0) {
            s_state.subscribed = false;
            return;
        }
    }
    /* HID hosts expect ~10 ms between reports for repeated keys. */
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* Map ASCII char to (modifier, keycode). Returns false if unsupported. */
static bool ascii_to_kc(char c, uint8_t *mod, uint8_t *kc)
{
    *mod = 0;
    if (c >= 'a' && c <= 'z') { *kc = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *mod = MOD_SHIFT_L; *kc = 0x04 + (c - 'A'); return true; }
    if (c >= '1' && c <= '9') { *kc = 0x1E + (c - '1'); return true; }
    if (c == '0') { *kc = 0x27; return true; }
    if (c == ' ') { *kc = KC_SPACE; return true; }
    if (c == '-') { *kc = 0x2D; return true; }
    if (c == '=') { *kc = 0x2E; return true; }
    if (c == '[') { *kc = 0x2F; return true; }
    if (c == ']') { *kc = 0x30; return true; }
    if (c == '\\') { *kc = 0x31; return true; }
    if (c == ';') { *kc = 0x33; return true; }
    if (c == '\'') { *kc = 0x34; return true; }
    if (c == ',') { *kc = 0x36; return true; }
    if (c == '.') { *kc = 0x37; return true; }
    if (c == '/') { *kc = 0x38; return true; }
    if (c == '`') { *kc = 0x35; return true; }
    if (c == '!') { *mod = MOD_SHIFT_L; *kc = 0x1E; return true; }
    if (c == '@') { *mod = MOD_SHIFT_L; *kc = 0x1F; return true; }
    if (c == '#') { *mod = MOD_SHIFT_L; *kc = 0x20; return true; }
    if (c == '$') { *mod = MOD_SHIFT_L; *kc = 0x21; return true; }
    if (c == '%') { *mod = MOD_SHIFT_L; *kc = 0x22; return true; }
    if (c == '&') { *mod = MOD_SHIFT_L; *kc = 0x24; return true; }
    if (c == '*') { *mod = MOD_SHIFT_L; *kc = 0x25; return true; }
    if (c == '(') { *mod = MOD_SHIFT_L; *kc = 0x26; return true; }
    if (c == ')') { *mod = MOD_SHIFT_L; *kc = 0x27; return true; }
    if (c == '_') { *mod = MOD_SHIFT_L; *kc = 0x2D; return true; }
    if (c == '+') { *mod = MOD_SHIFT_L; *kc = 0x2E; return true; }
    if (c == ':') { *mod = MOD_SHIFT_L; *kc = 0x33; return true; }
    if (c == '"') { *mod = MOD_SHIFT_L; *kc = 0x34; return true; }
    if (c == '?') { *mod = MOD_SHIFT_L; *kc = 0x38; return true; }
    return false;
}

static void type_string(const char *s)
{
    for (; *s; ++s) {
        uint8_t m, k;
        if (ascii_to_kc(*s, &m, &k)) {
            send_report(m, k);
        }
    }
}

/* --- DuckyScript interpreter ------------------------------------------- */

static uint32_t s_default_delay_ms = 0;

static void run_script(const char *src)
{
    /* Tokenizer: mutate a copy so strtok-style parse is safe. */
    size_t n = strlen(src);
    char *buf = malloc(n + 1);
    if (!buf) return;
    memcpy(buf, src, n + 1);

    /* Hak5 DuckyScript reference: DEFAULTDELAY is per-script. Reset so
     * a replay after a script that set DEFAULTDELAY does not inherit it.
     * https://docs.hak5.org/hak5-usb-rubber-ducky/ducky-script-basics/introduction */
    s_default_delay_ms = 0;

    char *line = buf;
    while (line && *line) {
        /* Bail early if the host disconnected or unsubscribed mid-run:
         * send_report would no-op for every remaining keystroke, but we
         * would still burn DELAY/DEFAULTDELAY ticks and CPU until EOF. */
        if (!s_state.subscribed) break;
        char *eol = strpbrk(line, "\r\n");
        char *next = eol ? eol + 1 : NULL;
        if (eol) *eol = '\0';

        /* skip leading whitespace */
        while (*line == ' ' || *line == '\t') ++line;
        if (*line == '\0' || strncmp(line, "REM", 3) == 0) {
            line = next;
            continue;
        }

        /* split first token */
        char *sp = strchr(line, ' ');
        char *arg = "";
        if (sp) { *sp = '\0'; arg = sp + 1; while (*arg == ' ') ++arg; }

        if (!strcmp(line, "DEFAULTDELAY") || !strcmp(line, "DEFAULT_DELAY")) {
            s_default_delay_ms = (uint32_t)strtoul(arg, NULL, 10);
        } else if (!strcmp(line, "DELAY")) {
            uint32_t ms = (uint32_t)strtoul(arg, NULL, 10);
            vTaskDelay(pdMS_TO_TICKS(ms));
        } else if (!strcmp(line, "STRING")) {
            type_string(arg);
        } else if (!strcmp(line, "ENTER"))     { send_report(0, KC_ENTER); }
        else if   (!strcmp(line, "TAB"))       { send_report(0, KC_TAB); }
        else if   (!strcmp(line, "SPACE"))     { send_report(0, KC_SPACE); }
        else if   (!strcmp(line, "BACKSPACE")) { send_report(0, KC_BACKSPACE); }
        else if   (!strcmp(line, "ESC") ||
                   !strcmp(line, "ESCAPE"))    { send_report(0, KC_ESC); }
        else if   (!strcmp(line, "DELETE"))    { send_report(0, KC_DELETE); }
        else if   (!strcmp(line, "F1") || !strcmp(line, "F2") || !strcmp(line, "F3") ||
                   !strcmp(line, "F4") || !strcmp(line, "F5") || !strcmp(line, "F6") ||
                   !strcmp(line, "F7") || !strcmp(line, "F8") || !strcmp(line, "F9") ||
                   !strcmp(line, "F10") || !strcmp(line, "F11") || !strcmp(line, "F12")) {
            int n = atoi(line + 1);
            send_report(0, KC_F1 + (n - 1));
        } else {
            /* Modifier combos: e.g. "CTRL ALT DEL", "GUI r", "SHIFT a". */
            uint8_t mod = 0;
            char *tok = line;
            while (tok) {
                char *next_tok = strchr(tok, ' ');
                if (next_tok) *next_tok++ = '\0';
                if (!strcmp(tok, "CTRL"))       mod |= MOD_CTRL_L;
                else if (!strcmp(tok, "SHIFT")) mod |= MOD_SHIFT_L;
                else if (!strcmp(tok, "ALT"))   mod |= MOD_ALT_L;
                else if (!strcmp(tok, "GUI") || !strcmp(tok, "WINDOWS") ||
                         !strcmp(tok, "WIN") || !strcmp(tok, "SUPER") ||
                         !strcmp(tok, "COMMAND")) mod |= MOD_GUI_L;
                else if (!strcmp(tok, "DELETE")) {
                    send_report(mod, KC_DELETE);
                    mod = 0; break;
                } else if (strlen(tok) == 1) {
                    uint8_t m2, k;
                    if (ascii_to_kc(tok[0], &m2, &k)) {
                        send_report(mod | m2, k);
                    }
                    mod = 0; break;
                }
                tok = next_tok;
                while (tok && *tok == ' ') ++tok;
            }
            if (mod != 0) {
                send_report(mod, 0);
            }
        }

        if (s_default_delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(s_default_delay_ms));
        }
        line = next;
    }
    free(buf);
}

static void runner_task(void *arg)
{
    (void)arg;
    while (true) {
        if (s_state.subscribed && !s_state.ran) {
            vTaskDelay(pdMS_TO_TICKS(s_state.start_delay_ms));
            if (s_state.subscribed) {
                size_t cap = 4096;
                char *payload = malloc(cap);
                if (payload) {
                    /* nvs_partition_gen writes [[inputs]] of type=string
                     * via nvs_set_str; we must read it as a string too.
                     * tw32_nvs_get_str NUL-terminates and bounds the
                     * read at cap-1. */
                    if (tw32_nvs_get_str("ducky", "payload", payload, cap)) {
                        run_script(payload);
                    }
                    free(payload);
                }
                s_state.ran = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* --- CLI ----------------------------------------------------------------- */

static int cmd_start_delay(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("start_delay", "missing_arg"); return -1; }
    uint32_t ms = (uint32_t)strtoul(argv[1], NULL, 10);
    if (ms > 600000) { tw32_cli_ack_err("start_delay", "out_of_range"); return -1; }
    s_state.start_delay_ms = ms;
    nvs_handle_t h;
    if (nvs_open("ducky", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "start_delay_ms", ms);
        nvs_commit(h); nvs_close(h);
    }
    tw32_cli_ack_ok("start_delay");
    return 0;
}

static int cmd_payload(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) { tw32_cli_ack_err("payload", "missing_subcmd"); return -1; }
    if (!strcmp(argv[1], "set")) {
        /* Reassemble the rest of argv as the payload (spaces preserved). */
        if (argc < 3) { tw32_cli_ack_err("payload", "empty"); return -1; }
        nvs_handle_t h;
        if (nvs_open("ducky", NVS_READWRITE, &h) != ESP_OK) {
            tw32_cli_ack_err("payload", "nvs_open"); return -1;
        }
        /* Concatenate argv[2..] joined with single spaces. The CLI
         * tokenizer already collapsed whitespace, so this is a faithful
         * reconstruction for newline-free payloads. For multi-line
         * scripts use the flasher's input pipeline instead.
         *
         * Bound the total at 3500 bytes: the ESP-IDF default NVS string
         * entry caps at ~4000 B (chunks of 32 B, one per blob entry) and
         * the runner reads with a 4096 B buffer; leave headroom for the
         * NUL and the trailing space separators. Reject oversize cleanly
         * instead of letting nvs_set_str return ESP_ERR_NVS_VALUE_TOO_LONG. */
        size_t tot = 0;
        for (int i = 2; i < argc; ++i) tot += strlen(argv[i]) + 1;
        if (tot > 3500) {
            nvs_close(h);
            tw32_cli_ack_err("payload", "too_long");
            return -1;
        }
        char *buf = malloc(tot + 1);
        if (!buf) { nvs_close(h); tw32_cli_ack_err("payload", "oom"); return -1; }
        size_t pos = 0;
        for (int i = 2; i < argc; ++i) {
            size_t L = strlen(argv[i]);
            memcpy(buf + pos, argv[i], L);
            pos += L;
            if (i + 1 < argc) buf[pos++] = ' ';
        }
        buf[pos] = '\0';
        esp_err_t e = nvs_set_str(h, "payload", buf);
        nvs_commit(h); nvs_close(h);
        free(buf);
        if (e == ESP_OK) {
            s_state.ran = false; /* allow re-run on next subscribe */
            tw32_cli_ack_ok("payload");
        } else {
            tw32_cli_ack_err("payload", "nvs_set");
        }
        return 0;
    }
    if (!strcmp(argv[1], "get")) {
        char buf[257];
        bool ok = tw32_nvs_get_str("ducky", "payload", buf, sizeof(buf));
        tw32_json_begin();
        tw32_json_kv_str ("cmd", "payload");
        tw32_json_kv_bool("ok", true);
        tw32_json_kv_str ("payload", ok ? buf : "");
        tw32_json_kv_uint("len", ok ? strlen(buf) : 0);
        tw32_json_end();
        return 0;
    }
    tw32_cli_ack_err("payload", "bad_subcmd");
    return -1;
}

static int cmd_replay(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    if (!s_state.subscribed) {
        tw32_cli_ack_err("replay", "not_subscribed");
        return -1;
    }
    s_state.ran = false;
    tw32_cli_ack_ok("replay");
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_bool("connected", s_state.connected);
    tw32_json_kv_bool("subscribed", s_state.subscribed);
    tw32_json_kv_bool("ran", s_state.ran);
    tw32_json_kv_uint("reports_sent", s_state.reports_sent);
    tw32_json_kv_uint("start_delay_ms", s_state.start_delay_ms);
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start_delay", "ms before payload runs after connect", cmd_start_delay },
    { "payload",     "payload set <text> | payload get",     cmd_payload     },
    { "replay",      "rerun payload while connected",        cmd_replay      },
    { "stats",       "connection state + counters",          cmd_stats       },
};

/* --- main --------------------------------------------------------------- */

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();

    /* Persisted start_delay_ms (range-checked). */
    uint32_t persisted = tw32_nvs_get_u32("ducky", "start_delay_ms", 0);
    if (persisted > 0 && persisted <= 600000) {
        s_state.start_delay_ms = persisted;
    }

    /* NimBLE bring-up: HID + battery + GAP services. */
    nimble_port_init();
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    /* Just-Works pairing with bonding. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = 1;
    ble_hs_cfg.sm_their_key_dist = 1;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    configASSERT(rc == 0);
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    configASSERT(rc == 0);

    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_svc_gap_device_appearance_set(HID_APPEARANCE);

    nimble_port_freertos_init(host_task);

    xTaskCreatePinnedToCore(runner_task, "tw32-ducky", 4096, NULL, 4, NULL, 1);

    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
