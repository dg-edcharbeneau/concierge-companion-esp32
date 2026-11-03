/*
 * Flux TTS voice catalog, and the one setting this device remembers.
 *
 * Two audiences for the same table. The model gets the `featured` subset named
 * in the set_voice description, and any one language's full list on request
 * through list_voices. The device uses the whole table to validate what comes
 * back and to turn a short name into the model id that goes on the wire.
 *
 * THE VOICE IMPLIES THE LANGUAGE
 *
 * Each voice belongs to one language, and choosing it chooses everything else
 * that depends on language: the listen model (flux-general-en, or
 * flux-general-multi hinted at that language), the line in the prompt that says
 * what to speak, and the greeting. There is no separate language setting to
 * disagree with the voice.
 *
 * Only languages the device can hear are here: the ten flux-general-multi
 * covers, plus Bulgarian, which only Nova-3 hears. Flux TTS speaks many more,
 * but a voice in a language the device cannot understand is a voice nobody can
 * talk back to. Japanese listens with Nova-3 too, because flux-general-multi
 * heard it too poorly; see `nova` and docs/notes/languages-probe.md.
 *
 * WHY THE VOICE IS NOT JUST A KCONFIG STRING ANY MORE
 *
 * Because dg_agent re-sends Settings on every WebSocket connect, a runtime value
 * survives reconnects and session restarts on its own. NVS is only needed to
 * carry it across a reboot.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

typedef struct {
    const char *code;      /* "es" -- the language_hints code and the model suffix */
    const char *name;      /* "Spanish" -- for the prompt and tool text */
    const char *greeting;  /* spoken on a fresh session; NULL = the Kconfig greeting */
    bool nova;             /* listen with Nova-3 in this language, not flux-general-multi */
    bool multi;            /* flux-general-multi hears it, so its fallback may hint it */
} language_t;

typedef struct {
    const char *name;   /* "hannah" -- the short name the model chooses by */
    const char *model;  /* "flux-hannah-en" -- what goes on the wire */
    const char *blurb;  /* "American woman, clear and confident" */
    const char *lang;   /* "en" -- a code in the language table */
    bool featured;      /* named in the set_voice description, vs list_voices only */
} voice_t;

/* Reads the saved voice, falling back to CONFIG_DEEPGRAM_FLUX_VOICE. Call once
 * at boot, before the first session. */
void voices_init(void);

/* Case-insensitive lookup by short name, and tolerant of the model id too, so
 * a model that says "flux-hannah-en" instead of "hannah" still works. NULL if
 * the name is not in the catalog. */
const voice_t *voices_find(const char *name);

/* The model id for the active voice -- what send_settings() and UpdateSpeak
 * both put on the wire. Never NULL. */
const char *voices_current_model(void);

/* The active voice's catalog entry. Never NULL. */
const voice_t *voices_current(void);

/* The language a voice speaks. Never NULL for a catalog entry. */
const language_t *voices_language(const voice_t *voice);

/* Looks a language up by code ("es"), case-insensitively. NULL if not offered. */
const language_t *voices_find_language(const char *code);

/* True for English, which keeps flux-general-en and the Kconfig greeting. */
bool voices_is_english(const language_t *lang);

/* Adopt a voice and persist it. */
esp_err_t voices_set(const voice_t *voice);

/* Forget the saved voice and go back to the Kconfig default. */
esp_err_t voices_reset(void);

/* The default the device falls back to. */
const voice_t *voices_default(void);

/* Appends a JSON string array of the language codes, for list_voices' `enum`. */
void voices_add_language_enum(cJSON *parent, const char *key);

/* Writes "hannah, kit, alexis, ..." -- the featured English voices, names only.
 * What each sounds like comes from list_voices, on request: the descriptions
 * were 575 B of every Settings message, and Settings is over the internal-heap
 * budget it is sent under. Truncates rather than overflowing. */
void voices_describe(char *out, size_t out_len);

/* Writes "Spanish (carmen), French (camille), ..." -- every language other
 * than English with its featured voice, so a request for "a French voice" needs
 * no list_voices round trip. No codes: list_voices' own enum carries those.
 * Same contract as voices_describe(). */
void voices_languages_describe(char *out, size_t out_len);

/* Writes "name (blurb), ..." for every voice in one language -- the list_voices
 * reply. Same contract as voices_describe(). */
void voices_list(const language_t *lang, char *out, size_t out_len);

/* "Flux" or "Nova-3" -- the {{listen_model}} placeholder, for when the model is
 * asked what it runs on. Same contract as voices_describe(). */
void voices_listen_model_name(char *out, size_t out_len);

/* The {{language}} line for the active voice: the instruction to speak its
 * language and to call set_voice for any other, with a leading space. Same contract as
 * voices_describe(), so it slots straight into agent_prompt.c's placeholder
 * table. */
void voices_language_note(char *out, size_t out_len);
