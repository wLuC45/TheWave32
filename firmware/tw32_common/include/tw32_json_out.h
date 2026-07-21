#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Streaming JSON-line emitter aimed at USB-CDC.
 *
 * Each "line" is a single JSON object terminated by '\n'. The emitter
 * builds the line in a small static buffer and flushes it in one
 * tw32_cdc_write so consumers can use line-buffered readers.
 *
 * Keys are not escaped — caller must use ASCII without quotes/backslash
 * (we always do). String values ARE escaped for ", \, and control
 * characters.
 *
 * Concurrency: an internal mutex serialises lines so multiple producer
 * tasks can emit safely.
 */

void tw32_json_begin(void);
void tw32_json_kv_str(const char *key, const char *val);
void tw32_json_kv_int(const char *key, int64_t val);
void tw32_json_kv_uint(const char *key, uint64_t val);
void tw32_json_kv_bool(const char *key, bool val);
void tw32_json_kv_mac(const char *key, const uint8_t mac[6]);

/* Begin / end an array value: tw32_json_array_begin("ssids");
 * tw32_json_array_str("foo"); tw32_json_array_str("bar"); tw32_json_array_end(); */
void tw32_json_array_begin(const char *key);
void tw32_json_array_str(const char *val);
void tw32_json_array_int(int64_t val);
void tw32_json_array_end(void);

/* Append the closing '}' + '\n' and ship the line via USB-CDC. */
void tw32_json_end(void);

#ifdef __cplusplus
}
#endif
