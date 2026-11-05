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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

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

/*
 * TWO SLOTS: one call in flight and one waiting behind it. A barge-in cancels
 * the pending call and the guest usually re-asks at once, so refusing a second
 * call outright turned every interruption into "the desk is busy" (seen on the
 * bench). Turns run one at a time -- the stay workflow orders them anyway, and a
 * second TLS session would double the internal-RAM cost.
 */
#define MAX_JOBS 2

typedef struct {
    bool in_use;
    bool cancelled;
    char session_tag[64];
    char id[96];
    char name[32];
    char turn_id[40];
    char *request;
    concierge_reply_fn reply;
} job_t;

static job_t s_jobs[MAX_JOBS];
static portMUX_TYPE s_jobs_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_run_lock;

#ifndef CONFIG_CONCIERGE_TURN_TIMEOUT_MS
#define CONFIG_CONCIERGE_TURN_TIMEOUT_MS 25000
#endif
/* A queued call waits out at most one full turn ahead of it, plus TLS setup. */
#define QUEUE_WAIT_MS (CONFIG_CONCIERGE_TURN_TIMEOUT_MS + 10000)

static bool job_cancelled(job_t *job)
{
    taskENTER_CRITICAL(&s_jobs_lock);
    bool c = job->cancelled;
    taskEXIT_CRITICAL(&s_jobs_lock);
    return c;
}

static void job_release(job_t *job)
{
    free(job->request);
    taskENTER_CRITICAL(&s_jobs_lock);
    memset(job, 0, sizeof(*job));
    taskEXIT_CRITICAL(&s_jobs_lock);
}

void concierge_tool_cancel(const char *id)
{
    if (id == NULL) {
        return;
    }
    bool hit = false;
    taskENTER_CRITICAL(&s_jobs_lock);
    for (int i = 0; i < MAX_JOBS; i++) {
        if (s_jobs[i].in_use && strcmp(s_jobs[i].id, id) == 0) {
            s_jobs[i].cancelled = true;
            hit = true;
        }
    }
    taskEXIT_CRITICAL(&s_jobs_lock);
    if (hit) {
        ESP_LOGI(TAG, "call %s cancelled by the agent", id);
    }
}

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
    const int64_t queued_us = esp_timer_get_time();

    if (xSemaphoreTake(s_run_lock, pdMS_TO_TICKS(QUEUE_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "turn %s gave up waiting for the previous one", job->turn_id);
        if (!job_cancelled(job)) {
            job->reply(job->session_tag, job->id, job->name, spoken_error(CONCIERGE_UNAVAILABLE));
        }
        job_release(job);
        vTaskDelete(NULL);
        return;
    }

    if (job_cancelled(job)) {
        /* Cancelled while queued: the guest has moved on, so no desk turn at all. */
        ESP_LOGI(TAG, "turn %s skipped: cancelled before it ran", job->turn_id);
    } else {
        const int64_t start_us = esp_timer_get_time();
        char *answer = NULL;
        concierge_status_t st = concierge_client_ask(job->turn_id, job->request, &answer);
        const int ms = (int)((esp_timer_get_time() - start_us) / 1000);
        const int waited = (int)((start_us - queued_us) / 1000);
        const bool cancelled = job_cancelled(job);
        if (st == CONCIERGE_OK) {
            ESP_LOGI(TAG, "turn %s answered in %d ms (queued %d ms, %u bytes)%s", job->turn_id, ms,
                     waited, (unsigned)strlen(answer), cancelled ? " -- cancelled, not sent" : "");
            if (!cancelled) {
                job->reply(job->session_tag, job->id, job->name, answer);
            }
        } else {
            ESP_LOGW(TAG, "turn %s failed in %d ms: status %d%s", job->turn_id, ms, (int)st,
                     cancelled ? " -- cancelled, not sent" : "");
            if (!cancelled) {
                job->reply(job->session_tag, job->id, job->name, spoken_error(st));
            }
        }
        free(answer);
    }
    xSemaphoreGive(s_run_lock);
    job_release(job);
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

    if (s_run_lock == NULL) {
        /* First call ever; FunctionCallRequests arrive on one task, so no race. */
        s_run_lock = xSemaphoreCreateMutex();
    }

    job_t *job = NULL;
    taskENTER_CRITICAL(&s_jobs_lock);
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!s_jobs[i].in_use) {
            job = &s_jobs[i];
            job->in_use = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_jobs_lock);
    if (job == NULL) {
        cJSON_Delete(args);
        reply(session_tag, id, name,
              "DESK ERROR: the desk is still answering earlier questions. Wait for "
              "those answers, then ask again.");
        return;
    }

    char *text = strdup(req->valuestring);
    cJSON_Delete(args);
    if (text == NULL || s_run_lock == NULL) {
        free(text);
        job_release(job);
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

    if (xTaskCreatePinnedToCore(worker, "concierge", TASK_STACK, job, TASK_PRIO, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "no internal RAM for the concierge task");
        reply(session_tag, id, name, spoken_error(CONCIERGE_UNAVAILABLE));
        job_release(job);
    }
}
