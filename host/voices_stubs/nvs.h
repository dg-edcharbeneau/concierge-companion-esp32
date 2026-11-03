/* Host stub: an NVS that never has anything saved and accepts every write. */
#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h)
{ (void)ns; (void)m; *h = 1; return ESP_OK; }
static inline esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *out, size_t *len)
{ (void)h; (void)k; (void)out; (void)len; return ESP_ERR_NVS_NOT_FOUND; }
static inline esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{ (void)h; (void)k; (void)v; return ESP_OK; }
static inline esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{ (void)h; (void)k; return ESP_OK; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static inline void nvs_close(nvs_handle_t h) { (void)h; }
