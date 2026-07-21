#include "tw32_io.h"

#include <string.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TW32_UART        UART_NUM_0
#define TW32_UART_BAUD   115200
/*
 * Deep TX ring so uart_write_bytes copies the line and returns instead of
 * blocking on a full ring. That matters because tw32_json_end() holds the
 * JSON assembly lock across the write: the only time that lock is held for
 * long is when the ring is full, so a larger ring keeps the critical
 * section short under bursts. (A full copy-out to drop the lock before the
 * write would touch every module's task stacks; deferred to a hardware
 * pass - see docs/wifi-clock-skew.md notes.)
 */
#define TW32_UART_TX_BUF 8192
#define TW32_UART_RX_BUF 1024

static SemaphoreHandle_t s_tx_lock;

void tw32_cdc_init(void)
{
    if (s_tx_lock != NULL) {
        return; /* idempotent */
    }
    s_tx_lock = xSemaphoreCreateMutex();
    configASSERT(s_tx_lock);

    const uart_config_t cfg = {
        .baud_rate = TW32_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /*
     * Default UART0 pins on ESP32-S3: TX=GPIO43, RX=GPIO44 — the same
     * pins routed to the CH343 on this dev board. Passing -1 keeps
     * the IDF defaults.
     */
    ESP_ERROR_CHECK(uart_driver_install(TW32_UART,
                                        TW32_UART_RX_BUF,
                                        TW32_UART_TX_BUF,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(TW32_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(TW32_UART, -1, -1, -1, -1));
}

size_t tw32_cdc_write(const void *buf, size_t len)
{
    if (!buf || len == 0) {
        return 0;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int written = uart_write_bytes(TW32_UART, (const char *)buf, len);
    xSemaphoreGive(s_tx_lock);
    return (written < 0) ? 0 : (size_t)written;
}

size_t tw32_cdc_write_str(const char *s)
{
    return s ? tw32_cdc_write(s, strlen(s)) : 0;
}

int tw32_cdc_readline(char *out, size_t cap, uint32_t timeout_ms)
{
    if (!out || cap == 0) {
        return -1;
    }
    size_t pos = 0;
    TickType_t to = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    while (pos + 1 < cap) {
        uint8_t c;
        int got = uart_read_bytes(TW32_UART, &c, 1, to);
        if (got <= 0) {
            if (pos == 0) {
                return 0; /* nothing yet */
            }
            break;
        }
        /*
         * Accept CR, LF, or CRLF as end-of-line so any of {tio, picocom,
         * screen, miniterm} works without per-tool config. We break on
         * either CR or LF and rely on the host sending a non-empty line:
         * the next CR/LF (in CRLF) hits the empty-line branch above (or
         * is consumed by a subsequent readline call returning 0 chars).
         */
        if (c == '\r' || c == '\n') {
            break;
        }
        out[pos++] = (char)c;
    }
    out[pos] = '\0';
    return (int)pos;
}
