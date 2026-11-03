#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "nvs.h"

#include "voices.h"

static const char *TAG = "voices";

/* Both inside NVS's 16-char limit for a namespace and a key. */
#define NVS_NAMESPACE "dgagent"
#define NVS_KEY_VOICE "tts_voice"

/* The buffer a saved model id is read back into. The longest are
 * "flux-veronique-fr", "flux-valentina-es" and friends at 17 chars;
 * host/voices_test.c holds every row to this. */
#define VOICE_MODEL_MAX 24

/*
 * The languages, in the order the model hears them listed. Each is one the
 * device can hear (flux-general-multi, or Nova-3 for a `nova` row) AND one the
 * Agent API took a Flux voice in when host/agent_probe.py last ran -- see
 * docs/notes/languages-probe.md. Adding one is a row here, its voices below,
 * and a re-run of the probe.
 *
 * The greetings stand in for CONFIG_DEEPGRAM_AGENT_GREETING, which is English
 * and would otherwise be the first thing a Spanish voice says. They want a
 * native speaker's eye before anyone relies on them.
 *
 * JAPANESE LISTENS WITH NOVA-3. On the same synthetic test set, streamed the way
 * the agent streams, flux-general-multi kept the meaning of 13 of 30 Japanese
 * commands and Nova-3 22 of 30 -- "capital" heard as "person", "volume up" as
 * "true feelings", alarms losing "tomorrow". The costs, also measured: Nova-3
 * signals the user starting to speak about a second later than Flux, so
 * barge-in is slower; and `language: "ja"` hears native English poorly or not
 * at all, so the prompt line tells the model what English looks like when it
 * does arrive. `multi` heard English fine but Japanese worse, which defeats the
 * point.
 *
 * BULGARIAN LISTENS WITH NOVA-3 BECAUSE NOTHING ELSE HEARS IT. Flux TTS has two
 * Bulgarian voices (Early Access); flux-general-multi does not take a "bg"
 * hint -- the Agent API answers INTERNAL_SERVER_ERROR -- and with only "en"
 * hinted it hears Bulgarian as English nonsense. Nova-3 `bg` heard the probe
 * question word for word. Its English is like Japanese's ("Speak English" as
 * "Спик English"), and `multi` false keeps "bg" out of the Nova-3 fallback's
 * hints, so a fallback session hears English only.
 */
static const language_t s_languages[] = {
    { "en", "English",    NULL,                                                         false, true },
    { "es", "Spanish",    "Aquí estoy. Háblame como a una persona.",                    false, true },
    { "fr", "French",     "Je suis là. Parle-moi simplement, comme à une personne.",    false, true },
    { "de", "German",     "Ich bin da. Sprich einfach mit mir wie mit einem Menschen.", false, true },
    { "hi", "Hindi",      "मैं यहाँ हूँ। मुझसे किसी इंसान की तरह बात करो।",                 false, true },
    { "ru", "Russian",    "Я здесь. Просто говори со мной, как с человеком.",           false, true },
    { "pt", "Portuguese", "Estou aqui. Fale comigo como se fala com uma pessoa.",       false, true },
    { "ja", "Japanese",   "はい、ここにいます。普通に話しかけてください。",                 true,  true },
    { "it", "Italian",    "Sono qui. Parlami come parleresti a una persona.",           false, true },
    { "nl", "Dutch",      "Ik ben er. Praat gewoon met me zoals met een mens.",         false, true },
    { "bg", "Bulgarian",  "Тук съм. Просто ми говори като на човек.",                   true,  false },
};

#define LANGUAGE_COUNT (sizeof(s_languages) / sizeof(s_languages[0]))

/*
 * The catalog, from Deepgram's Flux TTS voice list.
 *
 * `featured` marks the thirteen English voices Deepgram calls its strongest
 * all-rounders, plus one voice per other language -- the one the probe used, so
 * the voice a "French voice" request lands on is one that has been heard
 * working. Only those are named in the set_voice description, which rides in
 * every Settings message; the rest cost only rodata, and the model reaches them
 * through list_voices, whose reply is sent only when asked for.
 *
 * Deepgram gives only a gender for the non-English voices, so their blurbs say
 * no more than that, plus the variety where the catalog splits one.
 *
 * The model id is spelled out rather than derived from the name. It is what
 * goes on the wire, and one voice that ever breaks the flux-<name>-<lang>
 * pattern would turn a derivation into a silent bug. host/voices_test.c checks
 * the pattern instead, which catches a typo without trusting it.
 */
static const voice_t s_voices[] = {
    /* English, featured */
    { "hannah",    "flux-hannah-en",     "American woman, clear and confident",        "en", true  },
    { "kit",       "flux-kit-en",        "British man, friendly and energetic",        "en", true  },
    { "alexis",    "flux-alexis-en",     "American woman, professional and calm",      "en", true  },
    { "cliff",     "flux-cliff-en",      "American man, deep and raspy",               "en", true  },
    { "sienna",    "flux-sienna-en",     "American woman, warm and professional",      "en", true  },
    { "cole",      "flux-cole-en",       "American man, engaging and energetic",       "en", true  },
    { "brooke",    "flux-brooke-en",     "American woman, fast and confident",         "en", true  },
    { "colin",     "flux-colin-en",      "British man, warm and authoritative",        "en", true  },
    { "gemma",     "flux-gemma-en",      "British woman, kind and approachable",       "en", true  },
    { "haley",     "flux-haley-en",      "American woman, calm and empathetic",        "en", true  },
    { "heather",   "flux-heather-en",    "American woman, energetic and engaging",     "en", true  },
    { "miles",     "flux-miles-en",      "American man, calm and sincere",             "en", true  },
    { "sean",      "flux-sean-en",       "British man, kind and calming",              "en", true  },

    /* English, list_voices only. This is where most of the accent range lives. */
    { "bree",      "flux-bree-en",       "American woman, sweet and kind",             "en", false },
    { "brittany",  "flux-brittany-en",   "American woman, soft and confident",         "en", false },
    { "bruce",     "flux-bruce-en",      "American man, natural and engaged",          "en", false },
    { "conor",     "flux-conor-en",      "British man, deep and relaxed",              "en", false },
    { "donovan",   "flux-donovan-en",    "American man, professional and thoughtful",  "en", false },
    { "drew",      "flux-drew-en",       "American man, young and relaxed",            "en", false },
    { "elise",     "flux-elise-en",      "American woman, professional and caring",    "en", false },
    { "jack",      "flux-jack-en",       "British man, clear and professional",        "en", false },
    { "kai",       "flux-kai-en",        "Singaporean man, calm and knowledgeable",    "en", false },
    { "kelsey",    "flux-kelsey-en",     "American woman, calm and empathetic",        "en", false },
    { "maeve",     "flux-maeve-en",      "Irish woman, energetic and gentle",          "en", false },
    { "marcelo",   "flux-marcelo-en",    "Filipino man, calm and knowledgeable",       "en", false },
    { "marcus",    "flux-marcus-en",     "American man, smooth and helpful",           "en", false },
    { "meena",     "flux-meena-en",      "Indian woman, empathetic and reassuring",    "en", false },
    { "meghan",    "flux-meghan-en",     "American woman, energetic and kind",         "en", false },
    { "naveen",    "flux-naveen-en",     "Indian man, clear and knowledgeable",        "en", false },
    { "paige",     "flux-paige-en",      "American woman, calm and comfortable",       "en", false },
    { "priya",     "flux-priya-en",      "Indian woman, confident and reassuring",     "en", false },
    { "rufus",     "flux-rufus-en",      "British man, gentle and enthusiastic",       "en", false },
    { "sharon",    "flux-sharon-en",     "Australian woman, formal and relaxed",       "en", false },
    { "tanner",    "flux-tanner-en",     "British man, professional and calm",         "en", false },
    { "wade",      "flux-wade-en",       "American man, warm and enthusiastic",        "en", false },
    { "wes",       "flux-wes-en",        "American man, thoughtful and warm",          "en", false },

    /* Spanish */
    { "aina",      "flux-aina-es",       "Spanish woman",                              "es", false },
    { "alfredo",   "flux-alfredo-es",    "Spanish man",                                "es", false },
    { "carmen",    "flux-carmen-es",     "Spanish woman",                              "es", true  },
    { "cecilia",   "flux-cecilia-es",    "Spanish woman",                              "es", false },
    { "duvan",     "flux-duvan-es",      "Spanish man",                                "es", false },
    { "eduardo",   "flux-eduardo-es",    "Spanish man",                                "es", false },
    { "ernesto",   "flux-ernesto-es",    "Spanish man",                                "es", false },
    { "fernanda",  "flux-fernanda-es",   "Spanish woman",                              "es", false },
    { "fiorella",  "flux-fiorella-es",   "Spanish woman",                              "es", false },
    { "gustavo",   "flux-gustavo-es",    "Spanish man",                                "es", false },
    { "jaime",     "flux-jaime-es",      "Spanish man",                                "es", false },
    { "martin",    "flux-martin-es",     "Spanish man",                                "es", false },
    { "mauricio",  "flux-mauricio-es",   "Spanish man",                                "es", false },
    { "patricia",  "flux-patricia-es",   "Spanish woman",                              "es", false },
    { "pilar",     "flux-pilar-es",      "Spanish woman",                              "es", false },
    { "rafael",    "flux-rafael-es",     "Spanish man",                                "es", false },
    { "tomas",     "flux-tomas-es",      "Spanish man",                                "es", false },
    { "valentina", "flux-valentina-es",  "Spanish woman",                              "es", false },
    { "ysabel",    "flux-ysabel-es",     "Spanish woman",                              "es", false },

    /* French */
    { "arnaud",    "flux-arnaud-fr",     "French man",                                 "fr", false },
    { "benoit",    "flux-benoit-fr",     "French man",                                 "fr", false },
    { "brigitte",  "flux-brigitte-fr",   "French woman",                               "fr", false },
    { "camille",   "flux-camille-fr",    "French woman",                               "fr", true  },
    { "chloe",     "flux-chloe-fr",      "French woman",                               "fr", false },
    { "florian",   "flux-florian-fr",    "French man",                                 "fr", false },
    { "julien",    "flux-julien-fr",     "French man",                                 "fr", false },
    { "mathieu",   "flux-mathieu-fr",    "French man",                                 "fr", false },
    { "pierre",    "flux-pierre-fr",     "French man",                                 "fr", false },
    { "thierry",   "flux-thierry-fr",    "French man",                                 "fr", false },
    { "veronique", "flux-veronique-fr",  "French woman",                               "fr", false },
    { "yasmine",   "flux-yasmine-fr",    "French woman",                               "fr", false },

    /* German */
    { "benjamin",  "flux-benjamin-de",   "German man",                                 "de", false },
    { "christian", "flux-christian-de",  "German man",                                 "de", false },
    { "janine",    "flux-janine-de",     "German woman",                               "de", false },
    { "magdalena", "flux-magdalena-de",  "German woman",                               "de", false },
    { "moritz",    "flux-moritz-de",     "German man",                                 "de", false },
    { "nadine",    "flux-nadine-de",     "German woman",                               "de", false },
    { "rolf",      "flux-rolf-de",       "German man",                                 "de", false },
    { "sonja",     "flux-sonja-de",      "German woman",                               "de", true  },
    { "sven",      "flux-sven-de",       "German man",                                 "de", false },
    { "tobias",    "flux-tobias-de",     "German man",                                 "de", false },
    { "verena",    "flux-verena-de",     "German woman",                               "de", false },
    { "leonie",    "flux-leonie-de",     "Swiss German woman",                         "de", false },
    { "yannick",   "flux-yannick-de",    "Swiss German man",                           "de", false },

    /* Hindi */
    { "ajay",      "flux-ajay-hi",       "Hindi man",                                  "hi", false },
    { "anjali",    "flux-anjali-hi",     "Hindi woman",                                "hi", true  },
    { "arjun",     "flux-arjun-hi",      "Hindi man",                                  "hi", false },
    { "divya",     "flux-divya-hi",      "Hindi woman",                                "hi", false },
    { "jyoti",     "flux-jyoti-hi",      "Hindi woman",                                "hi", false },
    { "nikhil",    "flux-nikhil-hi",     "Hindi man",                                  "hi", false },
    { "pallavi",   "flux-pallavi-hi",    "Hindi woman",                                "hi", false },
    { "prachi",    "flux-prachi-hi",     "Hindi woman",                                "hi", false },
    { "rahul",     "flux-rahul-hi",      "Hindi man",                                  "hi", false },
    { "rajesh",    "flux-rajesh-hi",     "Hindi man",                                  "hi", false },
    { "ritu",      "flux-ritu-hi",       "Hindi woman",                                "hi", false },
    { "shreya",    "flux-shreya-hi",     "Hindi woman",                                "hi", false },
    { "sunita",    "flux-sunita-hi",     "Hindi woman",                                "hi", false },
    { "vikram",    "flux-vikram-hi",     "Hindi man",                                  "hi", false },

    /* Russian */
    { "andrei",    "flux-andrei-ru",     "Russian man",                                "ru", false },
    { "kirill",    "flux-kirill-ru",     "Russian man",                                "ru", false },
    { "maxim",     "flux-maxim-ru",      "Russian man",                                "ru", false },
    { "mikhail",   "flux-mikhail-ru",    "Russian man",                                "ru", false },
    { "natalia",   "flux-natalia-ru",    "Russian woman",                              "ru", false },
    { "nikolai",   "flux-nikolai-ru",    "Russian man",                                "ru", false },
    { "olga",      "flux-olga-ru",       "Russian woman",                              "ru", true  },
    { "polina",    "flux-polina-ru",     "Russian woman",                              "ru", false },
    { "vladimir",  "flux-vladimir-ru",   "Russian man",                                "ru", false },

    /* Portuguese */
    { "bianca",    "flux-bianca-pt",     "Brazilian Portuguese woman",                 "pt", true  },
    { "bruno",     "flux-bruno-pt",      "Brazilian Portuguese man",                   "pt", false },
    { "claudia",   "flux-claudia-pt",    "Brazilian Portuguese woman",                 "pt", false },
    { "leandro",   "flux-leandro-pt",    "Brazilian Portuguese man",                   "pt", false },
    { "marcia",    "flux-marcia-pt",     "Brazilian Portuguese woman",                 "pt", false },
    { "mariana",   "flux-mariana-pt",    "Brazilian Portuguese woman",                 "pt", false },
    { "marta",     "flux-marta-pt",      "Brazilian Portuguese woman",                 "pt", false },
    { "mateus",    "flux-mateus-pt",     "Brazilian Portuguese man",                   "pt", false },
    { "miguel",    "flux-miguel-pt",     "Brazilian Portuguese man",                   "pt", false },
    { "renato",    "flux-renato-pt",     "Brazilian Portuguese man",                   "pt", false },
    { "roberto",   "flux-roberto-pt",    "Brazilian Portuguese man",                   "pt", false },
    { "rodrigo",   "flux-rodrigo-pt",    "Brazilian Portuguese man",                   "pt", false },
    { "artur",     "flux-artur-pt",      "European Portuguese man",                    "pt", false },
    { "benedita",  "flux-benedita-pt",   "European Portuguese woman",                  "pt", false },
    { "catarina",  "flux-catarina-pt",   "European Portuguese woman",                  "pt", false },
    { "hugo",      "flux-hugo-pt",       "European Portuguese man",                    "pt", false },
    { "pedro",     "flux-pedro-pt",      "European Portuguese man",                    "pt", false },
    { "ricardo",   "flux-ricardo-pt",    "European Portuguese man",                    "pt", false },

    /* Japanese */
    { "ayumi",     "flux-ayumi-ja",      "Japanese woman",                             "ja", true  },
    { "hayato",    "flux-hayato-ja",     "Japanese man",                               "ja", false },
    { "nozomi",    "flux-nozomi-ja",     "Japanese woman",                             "ja", false },
    { "sayaka",    "flux-sayaka-ja",     "Japanese woman",                             "ja", false },

    /* Italian */
    { "angela",    "flux-angela-it",     "Italian woman",                              "it", false },
    { "chiara",    "flux-chiara-it",     "Italian woman",                              "it", false },
    { "federico",  "flux-federico-it",   "Italian man",                                "it", false },
    { "giorgio",   "flux-giorgio-it",    "Italian man",                                "it", false },
    { "matteo",    "flux-matteo-it",     "Italian man",                                "it", false },
    { "paola",     "flux-paola-it",      "Italian woman",                              "it", false },
    { "rocco",     "flux-rocco-it",      "Italian man",                                "it", false },
    { "sofia",     "flux-sofia-it",      "Italian woman",                              "it", true  },
    { "stefano",   "flux-stefano-it",    "Italian man",                                "it", false },
    { "vincenzo",  "flux-vincenzo-it",   "Italian man",                                "it", false },
    { "vito",      "flux-vito-it",       "Italian man",                                "it", false },

    /* Dutch */
    { "anouk",     "flux-anouk-nl",      "Dutch woman",                                "nl", true  },
    { "bram",      "flux-bram-nl",       "Dutch man",                                  "nl", false },
    { "fleur",     "flux-fleur-nl",      "Dutch woman",                                "nl", false },
    { "irene",     "flux-irene-nl",      "Dutch woman",                                "nl", false },
    { "luuk",      "flux-luuk-nl",       "Dutch man",                                  "nl", false },
    { "monique",   "flux-monique-nl",    "Dutch woman",                                "nl", false },
    { "ruben",     "flux-ruben-nl",      "Dutch man",                                  "nl", false },
    { "sanne",     "flux-sanne-nl",      "Dutch woman",                                "nl", false },
    { "yvonne",    "flux-yvonne-nl",     "Dutch woman",                                "nl", false },
    { "dirk",      "flux-dirk-nl",       "Flemish man",                                "nl", false },
    { "lotte",     "flux-lotte-nl",      "Flemish woman",                              "nl", false },
    { "niels",     "flux-niels-nl",      "Flemish man",                                "nl", false },
    { "veerle",    "flux-veerle-nl",     "Flemish woman",                              "nl", false },

    /* Bulgarian */
    { "desislava", "flux-desislava-bg",  "Bulgarian woman",                            "bg", true  },
    { "gergana",   "flux-gergana-bg",    "Bulgarian woman",                            "bg", false },
};

#define VOICE_COUNT (sizeof(s_voices) / sizeof(s_voices[0]))

/*
 * A row of the catalog, not a copy of a name: the table is rodata and outlives
 * everything, where the name the model sent arrives inside a cJSON tree that is
 * freed as soon as the message is handled. NULL until voices_init(), which
 * voices_current() covers with the default.
 *
 * Being the row also makes the voice, its model and its language one pointer
 * away. A copied model id had to be looked up again -- a case-insensitive scan
 * of the whole table -- for every placeholder and provider block in a Settings
 * message.
 */
static const voice_t *s_voice;

const voice_t *voices_find(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < VOICE_COUNT; i++) {
        /* Either spelling: the model is told to use short names, but it has the
         * full ids in front of it too and sometimes echoes those back. */
        if (strcasecmp(name, s_voices[i].name) == 0 ||
            strcasecmp(name, s_voices[i].model) == 0) {
            return &s_voices[i];
        }
    }
    return NULL;
}

const voice_t *voices_default(void)
{
    const voice_t *v = voices_find(CONFIG_DEEPGRAM_FLUX_VOICE);
    /* A Kconfig value outside the catalog is a build-time typo, but falling
     * back to the first entry keeps the device talking. */
    return (v != NULL) ? v : &s_voices[0];
}

const char *voices_current_model(void)
{
    return voices_current()->model;
}

void voices_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* NOT_FOUND here means the namespace has never been written, which is
         * every device until the first voice change. */
        s_voice = voices_default();
        ESP_LOGI(TAG, "no saved voice (%s), using default %s",
                 esp_err_to_name(err), s_voice->model);
        return;
    }

    char saved[VOICE_MODEL_MAX];
    size_t len = sizeof(saved);
    err = nvs_get_str(h, NVS_KEY_VOICE, saved, &len);
    nvs_close(h);

    const voice_t *v = (err == ESP_OK) ? voices_find(saved) : NULL;
    if (v == NULL) {
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "reading saved voice failed: %s", esp_err_to_name(err));
        }
        /* Also the path for a stored voice Deepgram has since retired. */
        s_voice = voices_default();
        ESP_LOGI(TAG, "voice: %s (default)", s_voice->model);
        return;
    }

    s_voice = v;
    ESP_LOGI(TAG, "voice: %s (saved)", s_voice->model);
}

esp_err_t voices_set(const voice_t *voice)
{
    if (voice == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_voice = voice;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_VOICE, voice->model);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        /* The session keeps the new voice either way -- only the next boot
         * loses it, which is worth a warning but not a failure. */
        ESP_LOGW(TAG, "could not persist voice: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "voice saved: %s", voice->model);
    }
    return err;
}

esp_err_t voices_reset(void)
{
    s_voice = voices_default();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, NVS_KEY_VOICE);
    /* Nothing saved is the same outcome as erasing it. */
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    ESP_LOGI(TAG, "voice reset to default: %s", s_voice->model);
    return err;
}

const voice_t *voices_current(void)
{
    return (s_voice != NULL) ? s_voice : voices_default();
}

const language_t *voices_find_language(const char *code)
{
    if (code == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < LANGUAGE_COUNT; i++) {
        if (strcasecmp(code, s_languages[i].code) == 0) {
            return &s_languages[i];
        }
    }
    return NULL;
}

const language_t *voices_language(const voice_t *voice)
{
    const language_t *l = (voice != NULL) ? voices_find_language(voice->lang) : NULL;
    /* Every row's lang is in the table -- host/voices_test.c holds it to that --
     * so English here is a fallback that should never be taken. */
    return (l != NULL) ? l : &s_languages[0];
}

bool voices_is_english(const language_t *lang)
{
    return lang == NULL || strcmp(lang->code, "en") == 0;
}

void voices_add_language_enum(cJSON *parent, const char *key)
{
    cJSON *arr = cJSON_AddArrayToObject(parent, key);
    if (arr == NULL) {
        return;
    }
    for (size_t i = 0; i < LANGUAGE_COUNT; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_languages[i].code));
    }
}

/*
 * One "a, b (detail), c" writer behind all three catalogs below, so the
 * truncation rule lives in one place. `detail` may be NULL for a bare name.
 * Returns false once the buffer is full; the caller stops.
 */
static bool append_item(char *out, size_t out_len, size_t *used,
                        const char *name, const char *detail)
{
    const char *sep = (*used > 0) ? ", " : "";
    int n = (detail != NULL)
        ? snprintf(out + *used, out_len - *used, "%s%s (%s)", sep, name, detail)
        : snprintf(out + *used, out_len - *used, "%s%s", sep, name);
    if (n < 0 || (size_t)n >= out_len - *used) {
        /* Truncated: better a short catalog than a corrupt one. */
        out[out_len - 1] = '\0';
        ESP_LOGW(TAG, "voice catalog text truncated");
        return false;
    }
    *used += (size_t)n;
    return true;
}

void voices_describe(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    size_t used = 0;
    out[0] = '\0';

    for (size_t i = 0; i < VOICE_COUNT; i++) {
        if (!s_voices[i].featured || strcmp(s_voices[i].lang, "en") != 0) {
            continue;
        }
        if (!append_item(out, out_len, &used, s_voices[i].name, NULL)) {
            return;
        }
    }
}

void voices_languages_describe(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    size_t used = 0;
    out[0] = '\0';

    for (size_t l = 0; l < LANGUAGE_COUNT; l++) {
        if (voices_is_english(&s_languages[l])) {
            continue;
        }
        for (size_t i = 0; i < VOICE_COUNT; i++) {
            if (s_voices[i].featured && strcmp(s_voices[i].lang, s_languages[l].code) == 0) {
                if (!append_item(out, out_len, &used, s_languages[l].name, s_voices[i].name)) {
                    return;
                }
                break;
            }
        }
    }
}

void voices_list(const language_t *lang, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    size_t used = 0;
    out[0] = '\0';
    if (lang == NULL) {
        return;
    }

    for (size_t i = 0; i < VOICE_COUNT; i++) {
        if (strcmp(s_voices[i].lang, lang->code) != 0) {
            continue;
        }
        if (!append_item(out, out_len, &used, s_voices[i].name, s_voices[i].blurb)) {
            return;
        }
    }
}

void voices_language_note(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    const language_t *l = voices_language(voices_current());
    /* English too. Without it, "Can you speak Spanish?" from an English
     * session got "Sí, claro" in the English voice and no set_voice: replaying
     * the board's own Settings from a laptop, 5 of 16 "Can you speak X?"
     * requests answered in X without changing voice, and 0 of 16 with this
     * line (2026-11-03). */
    /* English stays welcome because it stays in language_hints: "switch back to
     * English" has to be both heard and obeyed. */
    /* The leading space is the join: the placeholder sits straight after a
     * full stop. */
    /* Terse on purpose: this rides in every Settings message.
     *
     * "Only", and the way out named, because "Reply in Bulgarian unless asked
     * otherwise" read as leave to answer in English without changing voice:
     * on the board, "Английски" got "Okay, switching to English" in the
     * Bulgarian voice and no set_voice, twice. Replaying the board's own
     * Settings from a laptop, the old line missed 1 of 12 language changes
     * ("О, да, мога и на английски") and this one 0 of 12. */
    int n = snprintf(out, out_len, " Reply only in %s; for any other language, call "
                     "set_voice first.", l->name);
    if (l->nova && n > 0 && (size_t)n < out_len) {
        /* Nova-3 in `ja` spells English out ("Speak English" came back as
         * "peakenglish") or drops it, and in `bg` writes it half in Cyrillic
         * ("Спик English"); this is the user's only way home without a word
         * of either. */
        snprintf(out + n, out_len - (size_t)n,
                 " English may arrive garbled, like peakenglish; a request for "
                 "English means switch voice.");
    }
}

void voices_listen_model_name(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    snprintf(out, out_len, "%s", voices_language(voices_current())->nova ? "Nova-3" : "Flux");
}
