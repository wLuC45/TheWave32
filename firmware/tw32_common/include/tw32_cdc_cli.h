#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lightweight line-based CLI dispatcher running on USB-CDC.
 *
 * Each module registers a static command table and calls
 * tw32_cli_run(banner, table). The dispatch task is pinned to core 1
 * and only handles parsing + dispatch — actual work happens in module
 * tasks, signalled via the handler return value or shared state.
 *
 * Commands are matched on the first whitespace-delimited token.
 * Argument parsing is the handler's responsibility.
 */

typedef struct tw32_cli_ctx tw32_cli_ctx_t;
typedef int (*tw32_cli_fn_t)(tw32_cli_ctx_t *ctx, int argc, char **argv);

typedef struct {
    const char *name;       /* "start", "stop", … */
    const char *help;       /* one-line help shown by built-in `help` cmd */
    tw32_cli_fn_t fn;
} tw32_cli_cmd_t;

/* Helpers usable from a handler. They emit a single JSON-line ack
 * shaped {"cmd":"...","ok":true|false[,"err":"..."]}. */
void tw32_cli_ack_ok(const char *cmd);
void tw32_cli_ack_err(const char *cmd, const char *err);

/*
 * Print a startup banner ("tw32/<module> v<version> ready") and enter
 * the dispatch loop. Returns only on fatal error. Built-in commands
 * `help` and `version` are added automatically.
 *
 * `module_name` and `version` are stored for use by the `version`
 * built-in.
 */
void tw32_cli_run(const char *module_name,
                  const char *version,
                  const tw32_cli_cmd_t *table,
                  size_t table_len);

#ifdef __cplusplus
}
#endif
