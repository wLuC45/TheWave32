#pragma once

/*
 * Shared Wi-Fi scan helper for TheWave32 modules.
 *
 * Spawns one 6 KB worker task on core 1 (esp_wifi_scan_start blows the
 * 4 KB CLI task stack), exposes a blocking scan that returns up to 32
 * AP records, emits one ``{"event":"ap","idx":N,...}`` JSON line per
 * AP plus a terminating ``{"cmd":"scan","ok":true,"count":N,
 * "truncated":bool}``, and caches the result so subsequent commands
 * can resolve a 1-based index ("attack 3", "target 1") into the
 * corresponding AP without re-scanning.
 *
 * Usage from a module:
 *
 *     tw32_nvs_init();
 *     tw32_cdc_init();
 *     // bring up Wi-Fi in STA or APSTA mode + esp_wifi_start, optional
 *     // promiscuous mode if your module captures.
 *     tw32_wifi_scan_init();
 *
 *     // From a CLI handler:
 *     int n = tw32_wifi_scan_run();          // does the work + JSON
 *     // or
 *     tw32_ap_entry_t ap;
 *     if (tw32_wifi_scan_resolve_index(argv[1], &ap)) { ... }
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TW32_WIFI_SCAN_MAX 32
#define TW32_WIFI_SSID_MAX 32

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    int8_t  rssi;
    char    ssid[TW32_WIFI_SSID_MAX + 1];
    wifi_auth_mode_t authmode;     /* OPEN / WPA2 / WPA3 / OWE / ... */
} tw32_ap_entry_t;

/* Initialise the scan worker task. Idempotent — safe to call once at
 * boot from app_main. Must be called AFTER esp_wifi_start. */
void tw32_wifi_scan_init(void);

/* Trigger a blocking active scan, emit JSON for each AP + terminator,
 * and cache the result. Handles promiscuous-mode toggle automatically:
 * if the radio was promiscuous on entry, it is disabled for the scan
 * and re-enabled afterwards.
 *
 * Returns the number of APs emitted (0..TW32_WIFI_SCAN_MAX) on success,
 * or -1 on error (in which case nothing useful is emitted; caller
 * should ack_err to surface the failure). */
int tw32_wifi_scan_run(void);

/* If `s` is a pure decimal 1-based index that maps to a cached AP from
 * the most recent successful tw32_wifi_scan_run(), fill `out` and
 * return true. Returns false otherwise (not a number, out of range,
 * or no scan has run yet). */
bool tw32_wifi_scan_resolve_index(const char *s, tw32_ap_entry_t *out);

/* Silent variant: fill `records` (which must hold up to `max_records`
 * wifi_ap_record_t structs) without emitting any JSON or touching the
 * cache. Useful for internal lookups (e.g. attack-by-SSID). Returns
 * count or -1 on error. Note: the caller is responsible for any
 * promiscuous-mode toggling — this helper does not change radio state. */
int tw32_wifi_scan_silent(wifi_ap_record_t *records, size_t max_records);

/* Number of APs in the cache from the last successful scan; 0 if none. */
int tw32_wifi_scan_cache_count(void);

#ifdef __cplusplus
}
#endif
