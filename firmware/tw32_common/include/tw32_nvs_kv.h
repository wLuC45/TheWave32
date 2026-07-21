#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tiny typed wrappers over the NVS API. They open the namespace,
 * read/default, and close — the cost is negligible since module setup
 * is cold-path.
 *
 * All `default_…` parameters are returned as-is when the key is
 * missing. Other NVS errors are logged and the default is returned.
 */

void tw32_nvs_init(void);

bool     tw32_nvs_get_bool(const char *ns, const char *key, bool default_value);
uint32_t tw32_nvs_get_u32 (const char *ns, const char *key, uint32_t default_value);
int32_t  tw32_nvs_get_i32 (const char *ns, const char *key, int32_t default_value);

/* String accessors copy at most cap-1 bytes plus NUL. Returns true if
 * a value was found. */
bool tw32_nvs_get_str (const char *ns, const char *key, char *out, size_t cap);

/* Blob accessors copy up to *len bytes; *len is updated to actual size.
 * Returns true if a value was found. */
bool tw32_nvs_get_blob(const char *ns, const char *key, void *out, size_t *len);

#ifdef __cplusplus
}
#endif
