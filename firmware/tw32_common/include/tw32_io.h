#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host-side I/O for TheWave32 modules.
 *
 * On the v0rtex dev board, the only host-visible serial path is the
 * CH343 USB-UART bridge attached to the SoC's UART0 (the same channel
 * the flasher uses). This module owns UART0 at the IDF driver level
 * and exposes a clean line-based API that the CLI / JSON emitters use
 * for both control (RX) and data (TX). ESP-IDF console / logging is
 * disabled for UART0 in sdkconfig, so the stream is JSON-only after
 * boot.
 *
 * Naming note: the API is "tw32_cdc_*" elsewhere in the codebase; we
 * keep that prefix because the *contract* (line-based, exclusive
 * stream to the host) is identical to a USB-CDC channel even though
 * the transport is UART. If a board with native USB-CDC ships later,
 * only this file changes.
 */

/* Initialise UART0 (115200 8N1) and our internal buffers. */
void tw32_cdc_init(void);

/* Queue bytes for transmission. Returns bytes accepted (always == len
 * unless the driver reports an error). */
size_t tw32_cdc_write(const void *buf, size_t len);

/* Convenience: write a NUL-terminated string. */
size_t tw32_cdc_write_str(const char *s);

/*
 * Read one line (terminated by '\n' or '\r\n') into `out`, blocking
 * up to `timeout_ms` between bytes (or forever if timeout_ms == 0).
 * Returns the number of bytes written to `out` (excluding the
 * terminator). The output is NUL-terminated. On timeout with no
 * accumulated bytes returns 0; on overflow returns -1.
 */
int tw32_cdc_readline(char *out, size_t cap, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
