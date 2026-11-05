/*
 * Concierge settings in NVS. Same precedence rule as main/api_key.c: a value
 * saved through the portal wins, and the Kconfig value is only a first-boot seed.
 *
 * Read once at init (concierge_tool_init) and written only by the portal. The
 * guest's credential is no longer kept here -- see concierge_session_t -- but
 * a save still erases the "cred" key older firmware left behind.
 */
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "concierge_internal.h"

static const char *TAG = "concierge";
static const char *NS = "concierge";

static bool read_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK || out[0] == '\0') {
        out[0] = '\0';
        return false;
    }
    return true;
}

static void __attribute__((unused)) seed(char *out, size_t cap, const char *value)
{
    if (out[0] == '\0' && value != NULL) {
        strlcpy(out, value, cap);
    }
}

bool concierge_store_load_guest(concierge_guest_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        read_str(h, "url", out->base_url, sizeof(out->base_url));
        read_str(h, "last", out->last_name, sizeof(out->last_name));
        read_str(h, "room", out->room, sizeof(out->room));
        nvs_close(h);
    }
#if CONFIG_CONCIERGE_ENABLE
    seed(out->base_url, sizeof(out->base_url), CONFIG_CONCIERGE_BASE_URL);
    seed(out->last_name, sizeof(out->last_name), CONFIG_CONCIERGE_GUEST_LAST_NAME);
    seed(out->room, sizeof(out->room), CONFIG_CONCIERGE_GUEST_ROOM);
#endif
    /* A trailing slash would turn every path into "//api/...", which ASP.NET
     * routing does not match. */
    size_t n = strlen(out->base_url);
    while (n > 0 && out->base_url[n - 1] == '/') {
        out->base_url[--n] = '\0';
    }
    return out->base_url[0] != '\0' && out->last_name[0] != '\0' && out->room[0] != '\0';
}

bool concierge_store_is_configured(void)
{
    concierge_guest_t g;
    return concierge_store_load_guest(&g);
}

esp_err_t concierge_store_save(const char *base_url, const char *last_name, const char *room)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    bool changed = false;
    if (base_url != NULL && base_url[0] != '\0') {
        err = nvs_set_str(h, "url", base_url);
        changed = true;
    }
    if (err == ESP_OK && last_name != NULL && last_name[0] != '\0') {
        err = nvs_set_str(h, "last", last_name);
        changed = true;
    }
    if (err == ESP_OK && room != NULL && room[0] != '\0') {
        err = nvs_set_str(h, "room", room);
        changed = true;
    }
    /* A credential belongs to one guest at one concierge; any change orphans it. */
    if (err == ESP_OK && changed) {
        nvs_erase_key(h, "cred");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK && changed) {
        ESP_LOGI(TAG, "concierge settings saved");
    }
    return err;
}
