/*
 * The ask_concierge client-side function.
 *
 * The device's own think LLM decides when to call it; the concierge's stay
 * workflow (its own model, tools, ride booking and memory) produces the answer,
 * which goes back as the FunctionCallResponse content for the device LLM to say.
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "concierge_internal.h"

static const char *TAG = "concierge";

#define TOOL_NAME "ask_concierge"
/* ConciergeTurnLimits.MaxUserMessageUtf8Bytes */
#define MAX_REQUEST_BYTES 2000
/*
 * INTERNAL RAM, NOT PSRAM: the task writes NVS when it caches a credential, and
 * a PSRAM stack is unreachable while flash writes disable the cache. 6 kB is what
 * the WebSocket task needs for its own TLS handshake; this one does the same.
 * The task exists only while a call is in flight.
 */
#define TASK_STACK 6144
#define TASK_PRIO  4

/*
 * Terse on purpose: Settings already runs near the size at which sessions
 * flapped (see HISTORY_REPLAY_BYTES in main/dg_agent.c). The when-to-call
 * rules live in prompt/concierge_boundaries.md.
 */
static const char DEFINITION[] =
    "{\"name\":\"" TOOL_NAME "\",\"description\":\"Ask the hotel concierge desk. "
    "Use for the hotel, the guest's stay, nearby places, weather, routes and rides. "
    "Returns the desk's answer to say.\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"request\":{\"type\":\"string\",\"description\":\"The guest's "
    "words, close to verbatim\"}},\"required\":[\"request\"]}}";

typedef struct {
    char session_tag[64];
    char id[96];
    char name[32];
    char turn_id[40];
    char *request;
    concierge_reply_fn reply;
} job_t;

static volatile bool s_busy;

void concierge_tool_add_definition(cJSON *functions)
{
    cJSON *fn = cJSON_Parse(DEFINITION);
    if (fn != NULL) {
        cJSON_AddItemToArray(functions, fn);
    }
}

bool concierge_tool_is(const char *name)
{
    return name != NULL && strcmp(name, TOOL_NAME) == 0;
}

/* Prefixed so the prompt can tell the model these are device-side problems to
 * explain briefly, not concierge answers to read out. */
static const char *spoken_error(concierge_status_t st)
{
    switch (st) {
    case CONCIERGE_NOT_CONFIGURED:
        return "DESK ERROR: this device is not linked to a hotel yet. Setup needs the "
               "concierge address, the guest's last name and room number.";
    case CONCIERGE_UNREACHABLE:
        return "DESK ERROR: the device could not reach the concierge server. Check "
               "the Wi-Fi and that the concierge's public tunnel is running.";
    case CONCIERGE_GUEST_NOT_FOUND:
        return "DESK ERROR: no current stay matches the saved last name and room "
               "number. Run setup again with the guest's details.";
    case CONCIERGE_UNAUTHORIZED:
        return "DESK ERROR: the desk ended this device's session; the stay may have "
               "ended or been reset. Run setup again for a new stay.";
    case CONCIERGE_REJECTED:
        return "DESK ERROR: the desk could not take that request. Ask the guest to "
               "put it another way.";
    case CONCIERGE_UNAVAILABLE:
        return "DESK ERROR: the concierge is temporarily unavailable. Try again in "
               "a moment.";
    case CONCIERGE_BAD_RESPONSE:
    default:
        return "DESK ERROR: the desk replied in a form this device does not "
               "understand. The device and concierge versions may not match.";
    }
}

/* Cuts at a UTF-8 boundary so the API's byte limit never splits a character. */
static void clamp_utf8(char *s, size_t max_bytes)
{
    size_t n = strlen(s);
    if (n <= max_bytes) {
        return;
    }
    n = max_bytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
        n--;
    }
    s[n] = '\0';
}

static void worker(void *arg)
{
    job_t *job = arg;
    char *answer = NULL;
    concierge_status_t st = concierge_client_ask(job->turn_id, job->request, &answer);
    if (st == CONCIERGE_OK) {
        ESP_LOGI(TAG, "turn %s answered (%u bytes)", job->turn_id, (unsigned)strlen(answer));
        job->reply(job->session_tag, job->id, job->name, answer);
    } else {
        ESP_LOGW(TAG, "turn %s failed: status %d", job->turn_id, (int)st);
        job->reply(job->session_tag, job->id, job->name, spoken_error(st));
    }
    free(answer);
    free(job->request);
    free(job);
    s_busy = false;
    vTaskDelete(NULL);
}

void concierge_tool_handle_async(const char *session_tag, const char *id, const char *name,
                                 const char *arguments_json, concierge_reply_fn reply)
{
    cJSON *args = arguments_json ? cJSON_Parse(arguments_json) : NULL;
    const cJSON *req = args ? cJSON_GetObjectItemCaseSensitive(args, "request") : NULL;
    if (!cJSON_IsString(req) || req->valuestring[0] == '\0') {
        cJSON_Delete(args);
        reply(session_tag, id, name,
              "DESK ERROR: ask_concierge needs a non-empty request.");
        return;
    }

    /* One at a time: the stay workflow orders turns anyway, and a second TLS
     * session would double the internal-RAM cost for no gain. */
    if (s_busy) {
        cJSON_Delete(args);
        reply(session_tag, id, name,
              "DESK ERROR: the desk is still answering the previous question. "
              "Wait for that answer, then ask again.");
        return;
    }

    job_t *job = calloc(1, sizeof(*job));
    char *text = job ? strdup(req->valuestring) : NULL;
    cJSON_Delete(args);
    if (job == NULL || text == NULL) {
        free(job);
        free(text);
        reply(session_tag, id, name, spoken_error(CONCIERGE_UNAVAILABLE));
        return;
    }
    clamp_utf8(text, MAX_REQUEST_BYTES);
    strlcpy(job->session_tag, session_tag ? session_tag : "", sizeof(job->session_tag));
    strlcpy(job->id, id, sizeof(job->id));
    strlcpy(job->name, name, sizeof(job->name));
    /* Unique per call; the one 401 retry reuses it so the workflow joins. */
    snprintf(job->turn_id, sizeof(job->turn_id), "dev:%08lx%08lx%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random());
    job->request = text;
    job->reply = reply;

    s_busy = true;
    if (xTaskCreatePinnedToCore(worker, "concierge", TASK_STACK, job, TASK_PRIO, NULL, 0) != pdPASS) {
        s_busy = false;
        ESP_LOGE(TAG, "no internal RAM for the concierge task");
        reply(session_tag, id, name, spoken_error(CONCIERGE_UNAVAILABLE));
        free(job->request);
        free(job);
    }
}
