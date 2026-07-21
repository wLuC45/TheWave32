/*
 * TheWave32 / usb-hid-keyboard
 *
 * Same DuckyScript subset as ble-hid-keyboard, but the keystrokes are
 * delivered over the ESP32-S3's USB-OTG peripheral as a USB HID
 * keyboard — no pairing, no permission, instant on-plug. The host
 * sees a generic "TW32-USB Keyboard" device.
 *
 * The runtime CLI (UART0 / CH343) is unchanged: NVS payload set/get,
 * start_delay, replay, stats. Once `replay` is invoked while the host
 * is enumerated, the script types itself out.
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_nvs_kv.h"

#define MODULE_NAME    "usb-hid-keyboard"
#define MODULE_VERSION "0.2.0"

#define MOD_CTRL_L  (1 << 0)
#define MOD_SHIFT_L (1 << 1)
#define MOD_ALT_L   (1 << 2)
#define MOD_GUI_L   (1 << 3)

#define KC_ENTER     0x28
#define KC_ESC       0x29
#define KC_BACKSPACE 0x2A
#define KC_TAB       0x2B
#define KC_SPACE     0x2C
#define KC_DELETE    0x4C
#define KC_F1        0x3A

typedef struct {
    /* TinyUSB callbacks run on the TinyUSB task (typically core 0); the
     * runner task and CLI read these from core 1. `volatile` only
     * inhibits compiler caching, not cross-core ordering, so use C11
     * atomics. `_Atomic bool` is acceptable on Xtensa (1-byte aligned). */
    _Atomic bool      mounted;          /* USB enumerated */
    _Atomic bool      ran;
    /* Incremented by the report sender (runner task + CLI replay path),
     * read by the CLI in stats: atomic to avoid lost increments. */
    _Atomic uint32_t  reports_sent;
    _Atomic uint32_t  start_delay_ms;
} state_t;

static state_t s_state;

/* HID Report Descriptor — standard boot keyboard. */
static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

/* USB device descriptor. */
static const tusb_desc_device_t s_dev_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,           /* Espressif */
    .idProduct          = 0x4002,           /* TheWave32 / experimental */
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

/* String descriptor table — index matches iManufacturer / iProduct etc. */
static const char *s_string_desc[5] = {
    (const char[]){0x09, 0x04},   /* 0: lang en-US */
    "TheWave32",                  /* 1: manufacturer */
    "TW32-USB Keyboard",          /* 2: product */
    "TW32-001",                   /* 3: serial */
    "TW32 HID Interface",         /* 4: HID interface name */
};

#define EPNUM_HID 0x81
#define HID_INTERFACE_STR_IDX 0x04

/* Configuration descriptor: 1 config × 1 interface × HID class. */
static const uint8_t s_cfg_desc[TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0,
                          TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, HID_INTERFACE_STR_IDX,
                       HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_hid_report_desc),
                       EPNUM_HID, 16, 10),
};

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer; (void)bufsize;
}

void tud_mount_cb(void)
{
    atomic_store(&s_state.mounted, true);
}

void tud_umount_cb(void)
{
    atomic_store(&s_state.mounted, false);
    atomic_store(&s_state.ran, false);
}

/* --- HID send helper -----------------------------------------------
 *
 * The HID interrupt-IN endpoint has a single in-flight transfer at a
 * time; calling tud_hid_keyboard_report() while a previous report is
 * still being shipped returns false and drops the new report on the
 * floor (TinyUSB queues nothing). USB HID 1.11 sec 7.2 also requires a
 * release report to make held keys lift, so dropping the release here
 * would result in stuck keys on the host. Wait on tud_hid_ready() with
 * a bounded timeout before each call, and retry both press and release
 * a couple of times. The 10 ms bInterval in the config descriptor sets
 * a floor on host polling cadence; press+10ms+release+10ms is the
 * minimum safe spacing for held-key reports per the spec.
 */
static bool wait_hid_ready(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (!tud_hid_ready()) {
        if (!atomic_load(&s_state.mounted)) return false;
        if (waited >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(2));
        waited += 2;
    }
    return true;
}

static void send_report(uint8_t mods, uint8_t kc)
{
    if (!atomic_load(&s_state.mounted)) return;
    uint8_t key[6] = { kc, 0, 0, 0, 0, 0 };
    if (!wait_hid_ready(50)) return;
    if (tud_hid_keyboard_report(0 /* report_id */, mods, key)) {
        atomic_fetch_add(&s_state.reports_sent, 1u);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    /* Release. Retry once if the endpoint was still busy; a missed
     * release means a stuck modifier or key on the host. */
    uint8_t empty[6] = {0};
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!atomic_load(&s_state.mounted)) return;
        if (wait_hid_ready(50) && tud_hid_keyboard_report(0, 0, empty)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}

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
    return false;
}

static void type_string(const char *s)
{
    for (; *s; ++s) {
        uint8_t m, k;
        if (ascii_to_kc(*s, &m, &k)) send_report(m, k);
    }
}

static void run_script(const char *src)
{
    /* DEFAULTDELAY is per-script (Hak5 DuckyScript spec: applies for the
     * lifetime of the script unless overridden); a previous replay must
     * not bleed its DEFAULTDELAY into the next one. */
    uint32_t default_delay_ms = 0;

    size_t n = strlen(src);
    char *buf = malloc(n + 1);
    if (!buf) return;
    memcpy(buf, src, n + 1);
    char *line = buf;
    while (line && *line) {
        char *eol = strpbrk(line, "\r\n");
        char *next = eol ? eol + 1 : NULL;
        if (eol) *eol = '\0';
        while (*line == ' ' || *line == '\t') ++line;
        if (*line == '\0' || strncmp(line, "REM", 3) == 0) { line = next; continue; }
        char *sp = strchr(line, ' ');
        char *arg = "";
        if (sp) { *sp = '\0'; arg = sp + 1; while (*arg == ' ') ++arg; }
        if      (!strcmp(line, "DEFAULTDELAY") || !strcmp(line, "DEFAULT_DELAY")) default_delay_ms = (uint32_t)strtoul(arg, NULL, 10);
        else if (!strcmp(line, "DELAY"))    vTaskDelay(pdMS_TO_TICKS((uint32_t)strtoul(arg, NULL, 10)));
        else if (!strcmp(line, "STRING"))   type_string(arg);
        else if (!strcmp(line, "ENTER"))    send_report(0, KC_ENTER);
        else if (!strcmp(line, "TAB"))      send_report(0, KC_TAB);
        else if (!strcmp(line, "SPACE"))    send_report(0, KC_SPACE);
        else if (!strcmp(line, "BACKSPACE"))send_report(0, KC_BACKSPACE);
        else if (!strcmp(line, "ESC") || !strcmp(line, "ESCAPE")) send_report(0, KC_ESC);
        else if (!strcmp(line, "DELETE"))   send_report(0, KC_DELETE);
        else if (line[0] == 'F' && isdigit((unsigned char)line[1]) &&
                 (line[2] == '\0' || isdigit((unsigned char)line[2]))) {
            /* Accept F1..F12. Previous check rejected F10..F12 because
             * line[1] was always '1' for those. */
            int fn = atoi(line + 1);
            if (fn >= 1 && fn <= 12) send_report(0, KC_F1 + (fn - 1));
        } else {
            uint8_t mod = 0;
            char *tok = line;
            while (tok) {
                char *next_tok = strchr(tok, ' ');
                if (next_tok) *next_tok++ = '\0';
                if      (!strcmp(tok, "CTRL"))  mod |= MOD_CTRL_L;
                else if (!strcmp(tok, "SHIFT")) mod |= MOD_SHIFT_L;
                else if (!strcmp(tok, "ALT"))   mod |= MOD_ALT_L;
                else if (!strcmp(tok, "GUI") || !strcmp(tok, "WINDOWS") ||
                         !strcmp(tok, "WIN") || !strcmp(tok, "SUPER") ||
                         !strcmp(tok, "COMMAND")) mod |= MOD_GUI_L;
                else if (!strcmp(tok, "DELETE")) { send_report(mod, KC_DELETE); mod = 0; break; }
                else if (strlen(tok) == 1) {
                    uint8_t m2, k;
                    if (ascii_to_kc(tok[0], &m2, &k)) send_report(mod | m2, k);
                    mod = 0; break;
                }
                tok = next_tok;
                while (tok && *tok == ' ') ++tok;
            }
            if (mod != 0) send_report(mod, 0);
        }
        if (default_delay_ms) vTaskDelay(pdMS_TO_TICKS(default_delay_ms));
        line = next;
    }
    free(buf);
}

static void runner_task(void *arg)
{
    (void)arg;
    while (true) {
        if (atomic_load(&s_state.mounted) && !atomic_load(&s_state.ran)) {
            vTaskDelay(pdMS_TO_TICKS(atomic_load(&s_state.start_delay_ms)));
            if (atomic_load(&s_state.mounted)) {
                size_t cap = 4096;
                char *payload = malloc(cap);
                if (payload) {
                    if (tw32_nvs_get_str("ducky", "payload", payload, cap)) {
                        run_script(payload);
                    }
                    free(payload);
                }
                atomic_store(&s_state.ran, true);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* --- CLI ----------------------------------------------------------- */

static int cmd_start_delay(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("start_delay", "missing_arg"); return -1; }
    uint32_t ms = (uint32_t)strtoul(argv[1], NULL, 10);
    if (ms > 600000) { tw32_cli_ack_err("start_delay", "out_of_range"); return -1; }
    atomic_store(&s_state.start_delay_ms, ms);
    nvs_handle_t h;
    if (nvs_open("ducky", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "start_delay_ms", ms);
        nvs_commit(h); nvs_close(h);
    }
    tw32_cli_ack_ok("start_delay");
    return 0;
}

static int cmd_payload(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c;
    if (argc < 2) { tw32_cli_ack_err("payload", "missing_subcmd"); return -1; }
    if (!strcmp(argv[1], "set")) {
        if (argc < 3) { tw32_cli_ack_err("payload", "empty"); return -1; }
        /* NVS strings cap out around 4000 bytes before requiring blob
         * chaining; the runner reads with a 4096-byte buffer. Reject
         * oversized payloads up front rather than trip nvs_set_str. */
        size_t tot = 0;
        for (int i = 2; i < argc; ++i) tot += strlen(argv[i]) + 1;
        if (tot >= 4000) { tw32_cli_ack_err("payload", "too_long"); return -1; }
        nvs_handle_t h;
        if (nvs_open("ducky", NVS_READWRITE, &h) != ESP_OK) {
            tw32_cli_ack_err("payload", "nvs_open"); return -1;
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
        nvs_commit(h); nvs_close(h); free(buf);
        if (e == ESP_OK) { atomic_store(&s_state.ran, false); tw32_cli_ack_ok("payload"); }
        else             tw32_cli_ack_err("payload", "nvs_set");
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

static int cmd_replay(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    if (!atomic_load(&s_state.mounted)) { tw32_cli_ack_err("replay", "not_mounted"); return -1; }
    atomic_store(&s_state.ran, false);
    tw32_cli_ack_ok("replay");
    return 0;
}

static int cmd_stats(tw32_cli_ctx_t *c, int argc, char **argv)
{
    (void)c; (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str ("cmd", "stats");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_bool("mounted",        atomic_load(&s_state.mounted));
    tw32_json_kv_bool("ran",            atomic_load(&s_state.ran));
    tw32_json_kv_uint("reports_sent",   atomic_load(&s_state.reports_sent));
    tw32_json_kv_uint("start_delay_ms", atomic_load(&s_state.start_delay_ms));
    tw32_json_end();
    return 0;
}

static const tw32_cli_cmd_t cli_table[] = {
    { "start_delay", "ms before payload runs after USB enum", cmd_start_delay },
    { "payload",     "payload set <text> | payload get",      cmd_payload     },
    { "replay",      "rerun payload while USB host enumerated", cmd_replay    },
    { "stats",       "USB state + counters",                  cmd_stats       },
};

void app_main(void)
{
    tw32_nvs_init();
    tw32_cdc_init();

    /* Default 2 s startup delay (typical for Rubber Ducky payloads,
     * gives the host OS time to bind the HID interface). NVS overrides
     * it if the user persisted a value. */
    atomic_store(&s_state.start_delay_ms, 2000u);
    uint32_t persisted = tw32_nvs_get_u32("ducky", "start_delay_ms", 0);
    if (persisted > 0 && persisted <= 600000) atomic_store(&s_state.start_delay_ms, persisted);

    /* Bring up TinyUSB with HID-only descriptors. */
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor        = &s_dev_desc,
        .string_descriptor        = s_string_desc,
        .string_descriptor_count  = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
        .external_phy             = false,
        .configuration_descriptor = s_cfg_desc,
    };
    if (tinyusb_driver_install(&tusb_cfg) != ESP_OK) {
        /* Don't panic — let the CLI come up so the host can read stats
         * and discover the failure via JSON. mounted just stays false. */
    }

    xTaskCreatePinnedToCore(runner_task, "tw32-ducky-usb", 4096, NULL, 4, NULL, 1);

    tw32_cli_run(MODULE_NAME, MODULE_VERSION,
                 cli_table, sizeof(cli_table) / sizeof(cli_table[0]));
}
