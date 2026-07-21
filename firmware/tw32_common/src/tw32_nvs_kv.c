#include "tw32_nvs_kv.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "tw32-nvs";

void tw32_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS truncated; erasing and reinitialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

bool tw32_nvs_get_bool(const char *ns, const char *key, bool default_value)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        return default_value;
    }
    uint8_t v = default_value ? 1 : 0;
    nvs_get_u8(h, key, &v);
    nvs_close(h);
    return v != 0;
}

uint32_t tw32_nvs_get_u32(const char *ns, const char *key, uint32_t default_value)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        return default_value;
    }
    uint32_t v = default_value;
    nvs_get_u32(h, key, &v);
    nvs_close(h);
    return v;
}

int32_t tw32_nvs_get_i32(const char *ns, const char *key, int32_t default_value)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        return default_value;
    }
    int32_t v = default_value;
    nvs_get_i32(h, key, &v);
    nvs_close(h);
    return v;
}

bool tw32_nvs_get_str(const char *ns, const char *key, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    size_t got = cap;
    esp_err_t err = nvs_get_str(h, key, out, &got);
    nvs_close(h);
    if (err != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool tw32_nvs_get_blob(const char *ns, const char *key, void *out, size_t *len)
{
    if (!out || !len || *len == 0) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_blob(h, key, out, len);
    nvs_close(h);
    return err == ESP_OK;
}
