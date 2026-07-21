#pragma once

/*
 * Shared AP-security state + helpers for SoftAP-creating modules
 * (wifi-evil-twin, wifi-bssid-clone). Both modules originally carried
 * identical password/auth/hidden plumbing; this header centralises the
 * validation, the wifi_config_t.ap.* application, and the JSON stats
 * emission so the per-module .c files keep only the orchestration
 * logic that's actually different.
 *
 * Usage:
 *   1. Embed `tw32_ap_security_t sec;` in your module's state struct.
 *      Default-init to {} (zero) so authmode starts as WIFI_AUTH_OPEN.
 *   2. Call `tw32_ap_security_apply` after filling in
 *      wifi_config_t.ap.ssid/ssid_len/channel etc., before
 *      esp_wifi_set_config. It returns ESP_ERR_INVALID_ARG if WPA2
 *      was selected without a valid PSK length.
 *   3. Wire CLI command handlers via the `_set_*` setters; they
 *      validate, emit no JSON themselves — the caller wraps with
 *      tw32_cli_ack_ok / tw32_cli_ack_err.
 *   4. Call `tw32_ap_security_emit_stats` inside cmd_stats to add the
 *      `auth` / `has_password` / `hidden` keys to the current JSON
 *      object.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#define TW32_AP_PSK_MIN 8
#define TW32_AP_PSK_MAX 63

/* Size of the error-message buffer the setters expect from callers.
 * Longest message emitted today is "use_open_or_wpa2" (17 incl. NUL);
 * 24 leaves headroom without bloating the per-call-site stack. */
#define TW32_AP_ERROR_SIZE 24

typedef struct {
    wifi_auth_mode_t authmode;          /* OPEN or WPA2_PSK */
    char             psk[TW32_AP_PSK_MAX + 1];
    uint8_t          psk_len;
    bool             hidden;
} tw32_ap_security_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Apply the security fields to a partially-filled wifi_config_t (the
 * caller has already populated ssid/ssid_len/channel/max_connection
 * /beacon_interval). Returns ESP_OK or ESP_ERR_INVALID_ARG when WPA2
 * was requested without a valid PSK. */
esp_err_t tw32_ap_security_apply(const tw32_ap_security_t *sec,
                                 wifi_config_t *cfg);

/* Setters — return 0 on success, -1 on validation failure. The error
 * string is written into err_out (caller-allocated, TW32_AP_ERROR_SIZE
 * bytes). Writes are bounded via snprintf. */
int tw32_ap_security_set_password(tw32_ap_security_t *sec,
                                  const char *pwd,
                                  char err_out[TW32_AP_ERROR_SIZE]);
int tw32_ap_security_set_auth    (tw32_ap_security_t *sec,
                                  const char *mode,
                                  char err_out[TW32_AP_ERROR_SIZE]);
int tw32_ap_security_set_hidden  (tw32_ap_security_t *sec,
                                  const char *flag,
                                  char err_out[TW32_AP_ERROR_SIZE]);

/* Add `auth`, `has_password`, `hidden` keys to the current
 * tw32_json object (caller is between begin/end). */
void tw32_ap_security_emit_stats(const tw32_ap_security_t *sec);

#ifdef __cplusplus
}
#endif
