#include "tw32_cdc_cli.h"
#include "tw32_io.h"
#include "tw32_json_out.h"
#include "tw32_led.h"

#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Weak-linked tear-down hooks. Modules that link the Bluedroid or
 * NimBLE stacks provide the real symbols; others get the no-op stub at
 * link time. Same trick keeps tw32_common from forcing every module to
 * pull in the BT stack. */
extern int esp_bt_controller_disable(void) __attribute__((weak));
extern int esp_bt_controller_deinit(void)  __attribute__((weak));
extern int esp_bluedroid_disable(void)     __attribute__((weak));
extern int esp_bluedroid_deinit(void)      __attribute__((weak));
extern int nimble_port_stop(void)          __attribute__((weak));

#define MAX_LINE 256
#define MAX_ARGV 8

struct tw32_cli_ctx {
    const char *module_name;
    const char *version;
    const tw32_cli_cmd_t *table;
    size_t table_len;
};

static int builtin_help(tw32_cli_ctx_t *ctx, int argc, char **argv);
static int builtin_version(tw32_cli_ctx_t *ctx, int argc, char **argv);

void tw32_cli_ack_ok(const char *cmd)
{
    tw32_json_begin();
    tw32_json_kv_str("cmd", cmd);
    tw32_json_kv_bool("ok", true);
    tw32_json_end();

    /* Heuristic: which acknowledged commands transition the module's
     * "primary action" state? Used by the LED so the user sees a
     * fast colour cycle when something is actively running. */
    if (cmd) {
        if (!strcmp(cmd, "start")  || !strcmp(cmd, "scan")    ||
            !strcmp(cmd, "attack") || !strcmp(cmd, "connect") ||
            !strcmp(cmd, "replay")) {
            tw32_led_set_running(true);
        } else if (!strcmp(cmd, "stop") ||
                   !strcmp(cmd, "disconnect") ||
                   !strcmp(cmd, "clear")) {
            tw32_led_set_running(false);
        }
    }
}

void tw32_cli_ack_err(const char *cmd, const char *err)
{
    tw32_json_begin();
    tw32_json_kv_str("cmd", cmd);
    tw32_json_kv_bool("ok", false);
    tw32_json_kv_str("err", err);
    tw32_json_end();
}

static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') {
            *p++ = '\0';
        }
        if (!*p) {
            break;
        }
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            ++p;
        }
    }
    return argc;
}

static int builtin_help(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str("cmd", "help");
    tw32_json_kv_bool("ok", true);
    tw32_json_array_begin("commands");
    tw32_json_array_str("help");
    tw32_json_array_str("version");
    tw32_json_array_str("shutdown");
    for (size_t i = 0; i < ctx->table_len; ++i) {
        tw32_json_array_str(ctx->table[i].name);
    }
    tw32_json_array_end();
    tw32_json_end();
    return 0;
}

static int builtin_version(tw32_cli_ctx_t *ctx, int argc, char **argv)
{
    (void)argc; (void)argv;
    tw32_json_begin();
    tw32_json_kv_str("cmd", "version");
    tw32_json_kv_bool("ok", true);
    tw32_json_kv_str("module", ctx->module_name);
    tw32_json_kv_str("version", ctx->version);
    tw32_json_end();
    return 0;
}

static void cli_task(void *arg)
{
    tw32_cli_ctx_t *ctx = (tw32_cli_ctx_t *)arg;
    char line[MAX_LINE];
    char *argv[MAX_ARGV];

    /* Banner so the host knows we're alive. */
    tw32_json_begin();
    tw32_json_kv_str("event", "ready");
    tw32_json_kv_str("module", ctx->module_name);
    tw32_json_kv_str("version", ctx->version);
    tw32_json_end();

    while (true) {
        int n = tw32_cdc_readline(line, sizeof(line), 0 /* block forever */);
        if (n <= 0) {
            continue;
        }
        int argc = tokenize(line, argv, MAX_ARGV);
        if (argc == 0) {
            continue;
        }
        const char *cmd = argv[0];

        if (strcmp(cmd, "help") == 0) {
            builtin_help(ctx, argc, argv);
            continue;
        }
        if (strcmp(cmd, "version") == 0) {
            builtin_version(ctx, argc, argv);
            continue;
        }
        if (strcmp(cmd, "shutdown") == 0) {
            /* Power down the chip — it will draw ~10 µA in deep sleep
             * until the user presses the RESET button. The host can
             * use this as a soft "power off" since the ESP32 cannot
             * actually cut its own USB rail.
             *
             * Real-world gotcha: esp_deep_sleep_start asserts (or hangs)
             * if Wi-Fi or BT are still running. The shared CLI does not
             * know which subsystems each module uses, so we tear down
             * best-effort: weakly-linked BT hooks resolve to the real
             * stack on modules that pull it in, NULL otherwise; Wi-Fi
             * is always available because tw32_common requires it. */
            tw32_cli_ack_ok("shutdown");
            tw32_led_set_running(false);
            vTaskDelay(pdMS_TO_TICKS(50));

            /* Wi-Fi: harmless if not initialised — returns ESP_ERR_*. */
            esp_wifi_stop();
            esp_wifi_deinit();

            /* Bluetooth: only resolved when the module links the stack. */
            if (esp_bluedroid_disable)     esp_bluedroid_disable();
            if (esp_bluedroid_deinit)      esp_bluedroid_deinit();
            if (nimble_port_stop)          nimble_port_stop();
            if (esp_bt_controller_disable) esp_bt_controller_disable();
            if (esp_bt_controller_deinit)  esp_bt_controller_deinit();

            vTaskDelay(pdMS_TO_TICKS(80));
            esp_deep_sleep_start();
            continue;
        }

        bool found = false;
        for (size_t i = 0; i < ctx->table_len; ++i) {
            if (strcmp(cmd, ctx->table[i].name) == 0) {
                ctx->table[i].fn(ctx, argc, argv);
                found = true;
                break;
            }
        }
        if (!found) {
            tw32_cli_ack_err(cmd, "unknown_command");
        }
    }
}

void tw32_cli_run(const char *module_name,
                  const char *version,
                  const tw32_cli_cmd_t *table,
                  size_t table_len)
{
    static tw32_cli_ctx_t ctx;
    ctx.module_name = module_name;
    ctx.version = version;
    ctx.table = table;
    ctx.table_len = table_len;

    /* Heartbeat LED — best-effort, ignore failure so a board without
     * the GPIO 48 RGB LED still boots cleanly. The module name is
     * folded into the colour palette so each slug looks distinct. */
    tw32_led_init(module_name);

    /* Pin to core 1 so RF tasks (core 0) aren't perturbed by line parsing. */
    xTaskCreatePinnedToCore(cli_task, "tw32-cli", 4096, &ctx, 5, NULL, 1);
}
