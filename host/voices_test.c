/*
 * Checks main/voices.c's catalog on a laptop: a hundred-odd rows typed by hand,
 * where one slip -- a duplicated first name, a model id with the wrong language
 * suffix, a voice in a language the device cannot hear -- would ship silently
 * and surface as a voice change that does nothing.
 *
 * The real module is #included rather than linked, so the test can walk the
 * static tables directly. Only NVS, logging and cJSON are stubbed
 * (host/voices_stubs). Plain C99 plus strcasecmp, so it builds on glibc as
 * well as macOS -- voices.c deliberately has no strlcpy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../main/voices.c"

static int failures;

static void check(const char *what, int ok)
{
    printf("%-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        failures++;
    }
}

/* ---- the cJSON stub: records the enum voices_add_language_enum() builds ---- */

static const char *s_enum[32];
static int s_enum_len;
static cJSON s_node;

cJSON *cJSON_AddArrayToObject(cJSON *parent, const char *key)
{
    (void)parent;
    (void)key;
    s_enum_len = 0;
    return &s_node;
}

cJSON *cJSON_CreateString(const char *s)
{
    if (s_enum_len < (int)(sizeof(s_enum) / sizeof(s_enum[0]))) {
        s_enum[s_enum_len++] = s;
    }
    return &s_node;
}

int cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    (void)array;
    (void)item;
    return 1;
}

/* The ten languages flux-general-multi hears (docs: Flux language prompting).
 * A hint outside this list fails the session (a 400, or through the Agent API
 * INTERNAL_SERVER_ERROR), so a row's `multi` must match it exactly. */
static const char *const FLUX_STT[] = { "en", "es", "fr", "de", "hi", "ru", "pt", "ja", "it", "nl" };

static int flux_stt_hears(const char *code)
{
    for (size_t i = 0; i < sizeof(FLUX_STT) / sizeof(FLUX_STT[0]); i++) {
        if (strcmp(code, FLUX_STT[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    char buf[4096];

    /* ---- every row ---- */
    int names_unique = 1, model_pattern = 1, lang_known = 1, fits = 1, finds = 1;
    for (size_t i = 0; i < VOICE_COUNT; i++) {
        const voice_t *v = &s_voices[i];
        for (size_t j = i + 1; j < VOICE_COUNT; j++) {
            if (strcasecmp(v->name, s_voices[j].name) == 0) {
                printf("  duplicate short name: %s\n", v->name);
                names_unique = 0;
            }
        }
        snprintf(buf, sizeof(buf), "flux-%s-%s", v->name, v->lang);
        if (strcmp(buf, v->model) != 0) {
            printf("  %s: model is %s, pattern says %s\n", v->name, v->model, buf);
            model_pattern = 0;
        }
        if (voices_find_language(v->lang) == NULL) {
            printf("  %s: language %s is not in the table\n", v->name, v->lang);
            lang_known = 0;
        }
        /* voices_init() reads a saved id back into VOICE_MODEL_MAX bytes; a
         * longer one would fail the read and silently fall back to default. */
        voices_set(v);
        if (strlen(v->model) >= VOICE_MODEL_MAX || voices_current() != v) {
            printf("  %s: does not fit or does not stick\n", v->model);
            fits = 0;
        }
        if (voices_find(v->name) != v || voices_find(v->model) != v) {
            printf("  %s: voices_find() does not round-trip\n", v->name);
            finds = 0;
        }
    }
    check("short names are unique across every language", names_unique);
    check("every model is flux-<name>-<lang>", model_pattern);
    check("every voice's language is in the language table", lang_known);
    check("every model id fits the NVS read-back and sticks", fits);
    check("voices_find() finds each row by name and by model", finds);

    /* ---- every language ---- */
    int hears = 1, has_voice = 1, one_featured = 1, greets = 1;
    for (size_t l = 0; l < LANGUAGE_COUNT; l++) {
        const language_t *lang = &s_languages[l];
        int voices = 0, featured = 0;
        for (size_t i = 0; i < VOICE_COUNT; i++) {
            if (strcmp(s_voices[i].lang, lang->code) == 0) {
                voices++;
                featured += s_voices[i].featured;
            }
        }
        if (lang->multi != (bool)flux_stt_hears(lang->code)) {
            printf("  %s: `multi` says %d, flux-general-multi says %d\n", lang->code,
                   lang->multi, flux_stt_hears(lang->code));
            hears = 0;
        }
        if (!lang->multi && !lang->nova) {
            printf("  %s: neither flux-general-multi nor Nova-3 hears it\n", lang->code);
            hears = 0;
        }
        if (voices == 0) {
            printf("  %s: no voices\n", lang->code);
            has_voice = 0;
        }
        if (!voices_is_english(lang) && featured != 1) {
            printf("  %s: %d featured voices, want 1\n", lang->code, featured);
            one_featured = 0;
        }
        if (!voices_is_english(lang) && (lang->greeting == NULL || lang->greeting[0] == '\0')) {
            printf("  %s: no greeting\n", lang->code);
            greets = 0;
        }
    }
    check("every language is one the device can hear, `multi` exact", hears);
    check("every language has at least one voice", has_voice);
    check("every non-English language has exactly one featured voice", one_featured);
    check("every non-English language has its own greeting", greets);
    check("English is first, so it is the fallback", voices_is_english(&s_languages[0]));

    voices_add_language_enum(NULL, "enum");
    check("list_voices' enum carries every language",
          s_enum_len == (int)LANGUAGE_COUNT);

    /* ---- the text the model sees ---- */
    /* list_voices replies through a 2048 B buffer behind "<Language> voices: ". */
    int lists_fit = 1;
    for (size_t l = 0; l < LANGUAGE_COUNT; l++) {
        voices_list(&s_languages[l], buf, sizeof(buf));
        if (strlen(buf) + 32 >= 2048) {
            printf("  %s list is %zu bytes\n", s_languages[l].code, strlen(buf));
            lists_fit = 0;
        }
    }
    check("every language's list fits list_voices' buffer", lists_fit);

    /* set_voice's description: a ~60 B lead-in, both catalogs, a ~50 B tail,
     * all in VOICE_DESC_LEN = 2048 -- 170 and 50 are kept as headroom. */
    voices_describe(buf, sizeof(buf));
    size_t desc = strlen(buf);
    voices_languages_describe(buf, sizeof(buf));
    size_t langs = strlen(buf);
    printf("  set_voice catalogs: %zu + %zu bytes\n", desc, langs);
    check("set_voice's description fits its buffer", 170 + desc + langs + 50 < 2048);
    voices_describe(buf, sizeof(buf));
    check("the English list is names only",
          strncmp(buf, "hannah, kit, alexis", 19) == 0 && strchr(buf, '(') == NULL);
    voices_languages_describe(buf, sizeof(buf));
    check("the languages sentence names a voice for French",
          strstr(buf, "French (camille)") != NULL);

    char small[24];
    voices_list(&s_languages[0], small, sizeof(small));
    check("voices_list() truncates into a small buffer, terminated",
          strlen(small) < sizeof(small));

    /* ---- the {{language}} line ---- */
    voices_set(voices_find("kit"));
    voices_language_note(buf, sizeof(buf));
    check("English gets the line too: reply in English, set_voice for any other",
          strcmp(buf, " Reply only in English; for any other language, call "
                      "set_voice first.") == 0);

    voices_set(voices_find("carmen"));
    voices_language_note(buf, sizeof(buf));
    check("a Spanish voice tells the model to reply in Spanish",
          strncmp(buf, " Reply only in Spanish;", 23) == 0);
    check("and that another language means set_voice, not just other words",
          strstr(buf, "call set_voice first") != NULL);
    check("the current voice's language follows the voice",
          strcmp(voices_language(voices_current())->code, "es") == 0);

    voices_set(voices_find("ayumi"));
    voices_language_note(buf, sizeof(buf));
    check("Japanese tells the model what English looks like through Nova-3",
          strstr(buf, "peakenglish") != NULL);
    printf("  Japanese language line: %zu bytes\n", strlen(buf));
    voices_listen_model_name(buf, sizeof(buf));
    check("Japanese listens with Nova-3", strcmp(buf, "Nova-3") == 0);
    check("English never listens with Nova-3", !s_languages[0].nova);
    voices_set(voices_find("desislava"));
    voices_language_note(buf, sizeof(buf));
    check("Bulgarian tells the model what English looks like through Nova-3",
          strstr(buf, "peakenglish") != NULL);
    voices_listen_model_name(buf, sizeof(buf));
    check("Bulgarian listens with Nova-3", strcmp(buf, "Nova-3") == 0);

    /* apply_voice_in_place()'s UpdatePrompt line, built into LINE_LEN = 512 in
     * dg_agent.c: the sentence, the language note and the listen-model tail. */
    size_t longest = 0;
    for (size_t i = 0; i < VOICE_COUNT; i++) {
        voices_set(&s_voices[i]);
        voices_language_note(buf, sizeof(buf));
        size_t n = strlen("Voice change, replacing any earlier voice or language "
                          "instruction: you now speak as .") +
                   strlen(s_voices[i].name) + strlen(buf) +
                   strlen(" You now listen with Nova-3.");
        if (n > longest) {
            longest = n;
        }
    }
    printf("  longest in-place prompt line: %zu bytes\n", longest);
    check("every in-place prompt line fits dg_agent.c's 512 B", longest < 512);

    voices_reset();
    check("reset goes back to the Kconfig default",
          strcmp(voices_current_model(), "flux-kit-en") == 0);

    printf("%zu voices in %zu languages\n", VOICE_COUNT, LANGUAGE_COUNT);
    return failures ? 1 : 0;
}
