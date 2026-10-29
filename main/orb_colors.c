/*
 * The orb's colour catalog. See orb_colors.h.
 *
 * ORDER IS THE CONTRACT: index 0 is the boot default and what ui.c starts with.
 * Adding a colour means appending here; nothing else has a parallel table to keep
 * in step, which is the one way this is simpler than faces.
 */

#include <strings.h>
#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"

#include "orb_colors.h"

static const char *TAG = "orb_colors";

/* The namespace voices.c and audio_io.c use for the same kind of thing. */
#define NVS_NAMESPACE "dgagent"
#define NVS_KEY_COLOR "orb_color"

/*
 * The device's palette: Deepgram's brand colours (the brand starter kit's
 * "Color palette", the source of truth), the primary Fuchsia and the six
 * supporting colours, each a brand name over the plain name for its colour.
 * Names are the brand's, lowercased to match the other catalogs. The brand's
 * other two primaries and its functional colours are off-white, off-black,
 * black, grey and white, which are no orb colour.
 *
 * IN DIAL ORDER. The touch panel's picker sets them evenly round one dial,
 * clockwise from twelve o'clock, by index, so this order is the dial's: Fuchsia
 * at twelve, as the "Radial color picker" design puts it, then round.
 *
 * FUCHSIA IS FIRST, AND THE DEFAULT. Index 0 is what the device boots in and
 * what "put it back to normal" returns to. There is no white: the out-of-box
 * look is the primary colour, and every screen that follows the orb's colour
 * (ui_tone.h) follows it there.
 *
 * THE PLAIN NAME DOES TWO JOBS: it is what the picker prints under the brand
 * name, and what tells the model that "make it purple" means led. Every one goes
 * into every session's function schema, so they are one word each.
 */
static const orb_color_t s_colors[] = {
    { "fuchsia",  0xE042FF, "magenta" },
    { "clay",     0xEB2A2E, "red" },
    { "bakelite", 0xFA8100, "orange" },
    { "brass",    0xF7F300, "yellow" },
    { "phosphor", 0x7CD941, "green" },
    { "glass",    0x00B2FF, "blue" },
    { "led",      0x7F39FF, "purple" },
};

#define COLOR_COUNT (sizeof(s_colors) / sizeof(s_colors[0]))

size_t orb_colors_count(void)
{
    return COLOR_COUNT;
}

const char *orb_colors_name(size_t index)
{
    return (index < COLOR_COUNT) ? s_colors[index].name : NULL;
}

const char *orb_colors_plain(size_t index)
{
    return (index < COLOR_COUNT) ? s_colors[index].plain : NULL;
}

uint32_t orb_colors_rgb(size_t index)
{
    /* White rather than 0 on a bad index: black ink on the black ground would be
     * an invisible orb, which reads as a dead device rather than as a bug. */
    return (index < COLOR_COUNT) ? s_colors[index].rgb : 0xFFFFFFu;
}

int orb_colors_find(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < COLOR_COUNT; i++) {
        /* The plain name too: "purple" said back as the argument still lands. */
        if (strcasecmp(name, s_colors[i].name) == 0 || strcasecmp(name, s_colors[i].plain) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int orb_colors_saved(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return -1;   /* never written: every device until the first change */
    }
    char name[16];
    size_t len = sizeof(name);
    const esp_err_t err = nvs_get_str(h, NVS_KEY_COLOR, name, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return -1;
    }
    const int idx = orb_colors_find(name);
    ESP_LOGI(TAG, "orb colour: %s (%s)", name, idx >= 0 ? "saved" : "no longer in the catalog");
    return idx;
}

void orb_colors_save(size_t index)
{
    if (index >= COLOR_COUNT) {
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_COLOR, s_colors[index].name);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        /* The colour stays on screen either way; only the next boot loses it. */
        ESP_LOGW(TAG, "could not persist orb colour: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "EVT color saved %s", s_colors[index].name);
    }
}

void orb_colors_add_enum(cJSON *parent, const char *key)
{
    cJSON *arr = cJSON_AddArrayToObject(parent, key);
    if (arr == NULL) {
        return;
    }
    for (size_t i = 0; i < COLOR_COUNT; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_colors[i].name));
    }
}

void orb_colors_describe(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    size_t used = 0;
    out[0] = '\0';

    for (size_t i = 0; i < COLOR_COUNT; i++) {
        const char *sep = (used > 0) ? ", " : "";
        /* The default says so: it is where "back to normal" goes. */
        int n = snprintf(out + used, out_len - used, "%s%s (%s%s)", sep, s_colors[i].name,
                         s_colors[i].plain, i == 0 ? ", the default" : "");
        if (n < 0 || (size_t)n >= out_len - used) {
            /* Truncated: better a short catalog than a corrupt one. */
            out[out_len - 1] = '\0';
            ESP_LOGW(TAG, "colour description truncated");
            return;
        }
        used += (size_t)n;
    }
}
