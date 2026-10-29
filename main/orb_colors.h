/*
 * The orb's colour catalog.
 *
 * Two audiences for one table, exactly as faces.h and voices.h describe: the
 * model gets the names as an enum in a function-call schema so it can pick one
 * out loud, and the device uses the table to validate what comes back and turn a
 * name into an index.
 *
 * WHY COLOUR IS A uint32_t AND NOT AN lv_color_t
 *
 * dg_agent.c includes this header to build the schema, and the rule that keeps
 * "nothing outside ui.c calls lv_*" true is that the catalog headers stay free of
 * LVGL. So a colour is a plain 0xRRGGBB here and stays one until orb_raster.c,
 * which is the only file that has any business knowing about pixel formats.
 *
 * THE ORB AND THE SCREEN AROUND IT -- the words, clock, hints and touch panel
 * follow it by ui_tone.h's rules -- BUT NOT THE SPECTRUM. That face colours its
 * bands by frequency and by which half of the conversation is live, so a single
 * tint would destroy information rather than restyle it. The setting is stored
 * whichever face is up and shows on the orb.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

typedef struct {
    const char *name;  /* "clay" -- the brand name: the enum value, and the
                        * picker's big label */
    uint32_t rgb;      /* 0xRRGGBB, multiplicative on the orb's ink */
    const char *plain; /* "red" -- the colour in plain words: the picker's small
                        * label, and how the model maps what people say */
} orb_color_t;

/* How many colours exist. Index 0 is the boot default. */
size_t orb_colors_count(void);

/* Name for an index, or NULL if out of range. */
const char *orb_colors_name(size_t index);

/* The plain colour word for an index ("red"), or NULL if out of range. */
const char *orb_colors_plain(size_t index);

/* 0xRRGGBB for an index. Out of range returns white -- not in the palette, but a
 * bad index then shows a plain orb rather than an invisible black-on-black one. */
uint32_t orb_colors_rgb(size_t index);

/* Case-insensitive lookup by name or plain word. Returns the index, or -1. */
int orb_colors_find(const char *name);

/*
 * The colour chosen last, by voice or on the touch panel, kept in NVS by NAME
 * so a reordered catalog cannot turn someone's glass into something else. Returns
 * its index, or -1 for none saved (or a name no longer in the catalog).
 */
int orb_colors_saved(void);

/* Save `index` as the colour to boot with. A flash write: never from the LVGL
 * task -- main.c's loop calls it once a change has held for a moment. */
void orb_colors_save(size_t index);

/* Appends a JSON string array of the names, for the function schema's `enum`. */
void orb_colors_add_enum(cJSON *parent, const char *key);

/* Writes "fuschia (magenta, the default), clay (red), ..." for the function description,
 * because JSON Schema has nowhere to hang a per-value note. Truncates rather
 * than overflowing. */
void orb_colors_describe(char *out, size_t out_len);
