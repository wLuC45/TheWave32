#include "tw32_ap_security.h"
#include "tw32_json_out.h"

#include <stdio.h>
#include <string.h>

static inline void set_err(char *err_out, const char *msg)
{
    if (err_out) snprintf(err_out, TW32_AP_ERROR_SIZE, "%s", msg);
}

esp_err_t tw32_ap_security_apply(const tw32_ap_security_t *sec,
                                 wifi_config_t *cfg)
{
    if (sec == NULL || cfg == NULL) return ESP_ERR_INVALID_ARG;
    if (sec->authmode == WIFI_AUTH_WPA2_PSK &&
        sec->psk_len < TW32_AP_PSK_MIN) {
        return ESP_ERR_INVALID_ARG;
    }
    cfg->ap.authmode    = sec->authmode;
    cfg->ap.ssid_hidden = sec->hidden ? 1 : 0;
    if (sec->authmode == WIFI_AUTH_WPA2_PSK) {
        memcpy(cfg->ap.password, sec->psk, sec->psk_len);
        cfg->ap.password[sec->psk_len] = '\0';
        cfg->ap.pmf_cfg.required = false;
    }
    return ESP_OK;
}

int tw32_ap_security_set_password(tw32_ap_security_t *sec,
                                  const char *pwd,
                                  char err_out[TW32_AP_ERROR_SIZE])
{
    if (!pwd) {
        set_err(err_out, "missing_arg");
        return -1;
    }
    if (!strcmp(pwd, "clear")) {
        sec->psk_len = 0;
        sec->psk[0] = '\0';
        sec->authmode = WIFI_AUTH_OPEN;
        return 0;
    }
    size_t L = strlen(pwd);
    if (L < TW32_AP_PSK_MIN || L > TW32_AP_PSK_MAX) {
        set_err(err_out, "bad_length");
        return -1;
    }
    memcpy(sec->psk, pwd, L);
    sec->psk[L] = '\0';
    sec->psk_len = (uint8_t)L;
    /* Setting a password implicitly switches to WPA2_PSK so the user
     * doesn't have to also call `auth wpa2`. */
    sec->authmode = WIFI_AUTH_WPA2_PSK;
    return 0;
}

int tw32_ap_security_set_auth(tw32_ap_security_t *sec,
                              const char *mode,
                              char err_out[TW32_AP_ERROR_SIZE])
{
    if (!mode) {
        set_err(err_out, "missing_arg");
        return -1;
    }
    if (!strcmp(mode, "open")) {
        sec->authmode = WIFI_AUTH_OPEN;
        return 0;
    }
    if (!strcmp(mode, "wpa2")) {
        if (sec->psk_len < TW32_AP_PSK_MIN) {
            set_err(err_out, "no_password");
            return -1;
        }
        sec->authmode = WIFI_AUTH_WPA2_PSK;
        return 0;
    }
    set_err(err_out, "use_open_or_wpa2");
    return -1;
}

int tw32_ap_security_set_hidden(tw32_ap_security_t *sec,
                                const char *flag,
                                char err_out[TW32_AP_ERROR_SIZE])
{
    if (!flag) {
        set_err(err_out, "missing_arg");
        return -1;
    }
    if (!strcmp(flag, "on"))  { sec->hidden = true;  return 0; }
    if (!strcmp(flag, "off")) { sec->hidden = false; return 0; }
    set_err(err_out, "use_on_or_off");
    return -1;
}

void tw32_ap_security_emit_stats(const tw32_ap_security_t *sec)
{
    tw32_json_kv_str ("auth",
        sec->authmode == WIFI_AUTH_WPA2_PSK ? "wpa2" : "open");
    tw32_json_kv_bool("has_password", sec->psk_len > 0);
    tw32_json_kv_bool("hidden",       sec->hidden);
}
