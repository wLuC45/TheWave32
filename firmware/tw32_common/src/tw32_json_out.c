#include "tw32_json_out.h"
#include "tw32_io.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*
 * Buffer is sized for typical scanner records (~200 B). It is intentionally
 * not heap-backed so we never touch malloc on the hot path.
 */
#define JSON_BUF_BYTES 1024

static SemaphoreHandle_t s_lock;
static char s_buf[JSON_BUF_BYTES];
static size_t s_pos;
/* Tracks whether we already wrote an object/array entry, so the next
 * KV/array-element knows whether to prepend a comma. */
static bool s_need_comma;
static int s_array_depth;
/* Set when a record overflowed the line buffer. A truncated record is
 * structurally invalid JSON, so tw32_json_end() discards it and emits a
 * clean error line instead — keeping the host's line framing intact. */
static bool s_truncated;

static void ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

static void put(const char *s, size_t n)
{
    /* Reserve 2 bytes so tw32_json_end() can always terminate the line
     * with "}\n" — without that, an overflow drops the trailing newline
     * and the host's line reader merges this record with the next. */
    const size_t cap = sizeof(s_buf) - 2;
    if (s_pos + n > cap) {
        n = (s_pos < cap) ? cap - s_pos : 0;
        s_truncated = true;
    }
    memcpy(&s_buf[s_pos], s, n);
    s_pos += n;
}

static void put_c(char c) { put(&c, 1); }

static void put_cstr(const char *s) { put(s, strlen(s)); }

static void put_escaped(const char *s)
{
    put_c('"');
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            put_c('\\');
            put_c((char)c);
        } else if (c < 0x20) {
            char esc[7];
            int n = snprintf(esc, sizeof(esc), "\\u%04x", c);
            put(esc, (size_t)n);
        } else {
            put_c((char)c);
        }
    }
    put_c('"');
}

static void maybe_comma(void)
{
    if (s_need_comma) {
        put_c(',');
    }
    s_need_comma = true;
}

void tw32_json_begin(void)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pos = 0;
    s_need_comma = false;
    s_array_depth = 0;
    s_truncated = false;
    put_c('{');
}

void tw32_json_kv_str(const char *key, const char *val)
{
    maybe_comma();
    put_escaped(key);
    put_c(':');
    put_escaped(val ? val : "");
}

void tw32_json_kv_int(const char *key, int64_t val)
{
    maybe_comma();
    put_escaped(key);
    put_c(':');
    char num[24];
    int n = snprintf(num, sizeof(num), "%" PRId64, val);
    put(num, (size_t)n);
}

void tw32_json_kv_uint(const char *key, uint64_t val)
{
    maybe_comma();
    put_escaped(key);
    put_c(':');
    char num[24];
    int n = snprintf(num, sizeof(num), "%" PRIu64, val);
    put(num, (size_t)n);
}

void tw32_json_kv_bool(const char *key, bool val)
{
    maybe_comma();
    put_escaped(key);
    put_c(':');
    put_cstr(val ? "true" : "false");
}

void tw32_json_kv_mac(const char *key, const uint8_t mac[6])
{
    char s[18];
    snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    tw32_json_kv_str(key, s);
}

void tw32_json_array_begin(const char *key)
{
    maybe_comma();
    put_escaped(key);
    put_c(':');
    put_c('[');
    s_need_comma = false;
    s_array_depth++;
}

void tw32_json_array_str(const char *val)
{
    maybe_comma();
    put_escaped(val ? val : "");
}

void tw32_json_array_int(int64_t val)
{
    maybe_comma();
    char num[24];
    int n = snprintf(num, sizeof(num), "%" PRId64, val);
    put(num, (size_t)n);
}

void tw32_json_array_end(void)
{
    put_c(']');
    s_array_depth--;
    /* After closing an array, we DO need a comma before the next key. */
    s_need_comma = true;
}

void tw32_json_end(void)
{
    if (s_truncated) {
        /* The record overflowed the line buffer; whatever is in s_buf
         * is unbalanced JSON. Ship a clean, parseable error line so the
         * host sees a delimiter and a reason instead of garbage. */
        static const char err[] =
            "{\"event\":\"error\",\"err\":\"json_overflow\"}\n";
        tw32_cdc_write(err, sizeof(err) - 1);
    } else {
        /* put() reserved 2 bytes for exactly this. */
        s_buf[s_pos++] = '}';
        s_buf[s_pos++] = '\n';
        tw32_cdc_write(s_buf, s_pos);
    }
    s_pos = 0;
    s_need_comma = false;
    s_truncated = false;
    xSemaphoreGive(s_lock);
}
