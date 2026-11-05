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
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "concierge_hold.h"
#include "concierge_internal.h"
#include "esp_heap_caps.h"

static const char *TAG = "concierge";

#define TOOL_NAME "ask_concierge"
/* ConciergeTurnLimits.MaxUserMessageUtf8Bytes */
#define MAX_REQUEST_BYTES 2000
/*
 * ONE PERSISTENT WORKER, STACK AND TCB IN PSRAM. It used to be a task created
 * per call on a 6 kB INTERNAL stack, the companion's largest internal-RAM
 * transient, at a time the largest free internal block had been seen at 1.5 kB
 * on weak Wi-Fi (bench, 2026-11-04). PSRAM is plentiful, so the stack is roomier
 * for the TLS handshake. The price is the rule for PSRAM-stack tasks: no flash
 * operations, so the worker never touches NVS (see concierge_session_t).
 */
#define TASK_STACK 8192
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
    int64_t queued_us;
    char session_tag[64];
    char id[96];
    char name[32];
    char turn_id[40];
    char *request;
    concierge_reply_fn reply;
} job_t;

static void worker(void *arg);

/* Both in PSRAM, allocated once by concierge_tool_init(). */
static job_t *s_jobs;
static concierge_session_t *s_session;
static QueueHandle_t s_job_q;

/* ---------------- hold lines and recovery (concierge_hold.c) ----------------
 *
 * MEMORY: on a session the largest free internal block has been seen at 8.7 KB,
 * so this adds no task and allocates nothing per turn. One state struct and one
 * static mutex (internal, ~140 B together), one esp_timer created once (~60 B
 * internal heap), and the 256 B text buffer in PSRAM. The phrase pools are
 * rodata. Timer callbacks run on the shared esp_timer task (3.5 kB stack) and
 * do one state step plus a copy into the agent's existing inject slots.
 *
 * A MUTEX, NOT A SPINLOCK: each step and its esp_timer start/stop must happen
 * together, or two tasks could reorder an arm and a disarm. esp_timer calls are
 * not allowed inside a critical section.
 */
#ifndef CONFIG_CONCIERGE_HOLD_DELAY_MS
#define CONFIG_CONCIERGE_HOLD_DELAY_MS 2500
#define CONFIG_CONCIERGE_HOLD_LONG_MS 8000
#define CONFIG_CONCIERGE_RECOVERY_MS 1200
#endif

static concierge_agent_hooks_t s_hooks;
static hold_state_t s_hold;
static StaticSemaphore_t s_hold_mutex_mem;
static SemaphoreHandle_t s_hold_mutex;
static esp_timer_handle_t s_hold_timer;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Caller holds s_hold_mutex. */
static void hold_apply(const hold_step_t *s)
{
    if (s->timer_ms == 0) {
        esp_timer_stop(s_hold_timer); /* ESP_ERR_INVALID_STATE when idle: fine */
    } else if (s->timer_ms > 0) {
        esp_timer_stop(s_hold_timer);
        esp_timer_start_once(s_hold_timer, (uint64_t)s->timer_ms * 1000);
    }
    switch (s->note) {
    case HOLD_NOTE_HOLD_LINE:
        ESP_LOGI(TAG, "EVT hold line after %u ms: \"%s\"", (unsigned)s->elapsed_ms, s->text);
        break;
    case HOLD_NOTE_LONG_LINE:
        ESP_LOGI(TAG, "EVT still-waiting line after %u ms: \"%s\"", (unsigned)s->elapsed_ms, s->text);
        break;
    case HOLD_NOTE_HOLD_SKIPPED:
        ESP_LOGI(TAG, "EVT hold skipped (answered in %u ms)", (unsigned)s->elapsed_ms);
        break;
    case HOLD_NOTE_HOLD_COVERED:
        ESP_LOGI(TAG, "EVT hold not needed: the agent already spoke");
        break;
    case HOLD_NOTE_RECOVERY:
        ESP_LOGW(TAG, "EVT recovery: promised without a call; nudging");
        break;
    default:
        break;
    }
    if (s->act == HOLD_ACT_SAY && s_hooks.say != NULL) {
        s_hooks.say(s->text);
    } else if (s->act == HOLD_ACT_NUDGE && s_hooks.nudge != NULL) {
        s_hooks.nudge(s->text);
    }
}

/* Runs `expr` (an expression yielding hold_step_t) under the mutex and applies it. */
#define HOLD_STEP(expr)                                       \
    do {                                                      \
        if (s_hold_mutex != NULL &&                           \
            xSemaphoreTake(s_hold_mutex, portMAX_DELAY) == pdTRUE) { \
            hold_step_t step_ = (expr);                       \
            hold_apply(&step_);                               \
            xSemaphoreGive(s_hold_mutex);                     \
        }                                                     \
    } while (0)

static void hold_timer_cb(void *arg)
{
    (void)arg;
    const bool ready = (s_hooks.ready != NULL) && s_hooks.ready();
    HOLD_STEP(hold_on_timer(&s_hold, now_ms(), ready));
}

static void worker_start(void)
{
    s_session = heap_caps_calloc(1, sizeof(*s_session), MALLOC_CAP_SPIRAM);
    s_jobs = heap_caps_calloc(MAX_JOBS, sizeof(*s_jobs), MALLOC_CAP_SPIRAM);
    QueueHandle_t q = xQueueCreateWithCaps(MAX_JOBS, sizeof(job_t *), MALLOC_CAP_SPIRAM);
    if (s_session == NULL || s_jobs == NULL || q == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the concierge worker; ask_concierge will report the desk unavailable");
        return;
    }
    /* The one NVS read, here on the caller's internal stack -- never on the worker's. */
    s_session->configured = concierge_store_load_guest(&s_session->guest);
    if (!s_session->configured) {
        ESP_LOGW(TAG, "concierge address, last name or room not set; run the setup portal");
    }
    if (xTaskCreatePinnedToCoreWithCaps(worker, "concierge", TASK_STACK, q, TASK_PRIO, NULL, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "concierge worker not created; ask_concierge will report the desk unavailable");
        vQueueDeleteWithCaps(q);
        return;
    }
    s_job_q = q;
}

void concierge_tool_init(const concierge_agent_hooks_t *hooks)
{
    if (s_hold_mutex != NULL || hooks == NULL) {
        return;
    }
    s_hooks = *hooks;
    worker_start();
    /* PSRAM: recovery is the only user, and it works without it (disabled). */
    char *buf = heap_caps_malloc(HOLD_BUF_LEN, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGW(TAG, "no PSRAM for the hold buffer; recovery disabled");
    }
    const hold_config_t cfg = {
        .hold_delay_ms = CONFIG_CONCIERGE_HOLD_DELAY_MS,
        .hold_long_ms = CONFIG_CONCIERGE_HOLD_LONG_MS,
        .recovery_ms = CONFIG_CONCIERGE_RECOVERY_MS,
    };
    hold_init(&s_hold, &cfg, buf, esp_random());

    const esp_timer_create_args_t args = { .callback = hold_timer_cb, .name = "cg_hold" };
    if (esp_timer_create(&args, &s_hold_timer) != ESP_OK) {
        ESP_LOGW(TAG, "no hold timer; hold lines and recovery disabled");
        free(buf);
        return;
    }
    s_hold_mutex = xSemaphoreCreateMutexStatic(&s_hold_mutex_mem);
}

void concierge_on_function_call(const char *name)
{
    (void)name;
    HOLD_STEP(hold_on_function_call(&s_hold));
}

bool concierge_on_text(const char *role, const char *content)
{
    bool device_line = false;
    if (role == NULL || content == NULL || s_hold_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_hold_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (strcmp(role, "assistant") == 0) {
        device_line = hold_on_assistant_text(&s_hold, content);
    } else if (strcmp(role, "user") == 0) {
        hold_step_t step = hold_on_user_text(&s_hold, content);
        hold_apply(&step);
    }
    xSemaphoreGive(s_hold_mutex);
    return device_line;
}

void concierge_on_user_speaking(void)
{
    HOLD_STEP(hold_on_user_speaking(&s_hold));
}

void concierge_on_agent_done(void)
{
    HOLD_STEP(hold_on_agent_done(&s_hold, now_ms()));
}
static portMUX_TYPE s_jobs_lock = portMUX_INITIALIZER_UNLOCKED;

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
    if (s_jobs == NULL) {
        return;
    }
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
        if (s_hold_mutex != NULL && xSemaphoreTake(s_hold_mutex, portMAX_DELAY) == pdTRUE) {
            hold_on_call_cancel(&s_hold);
            xSemaphoreGive(s_hold_mutex);
        }
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

static void run_job(job_t *job)
{
    const int64_t start_us = esp_timer_get_time();
    const int waited = (int)((start_us - job->queued_us) / 1000);

    if (job_cancelled(job)) {
        /* Cancelled while queued: the guest has moved on, so no desk turn at all. */
        ESP_LOGI(TAG, "turn %s skipped: cancelled before it ran", job->turn_id);
        return;
    }
    if (waited > QUEUE_WAIT_MS) {
        ESP_LOGW(TAG, "turn %s gave up after waiting %d ms behind the previous one",
                 job->turn_id, waited);
        job->reply(job->session_tag, job->id, job->name, spoken_error(CONCIERGE_UNAVAILABLE));
        return;
    }

    char *answer = NULL;
    concierge_status_t st = concierge_client_ask(s_session, job->turn_id, job->request, &answer);
    const int ms = (int)((esp_timer_get_time() - start_us) / 1000);
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

static void worker(void *arg)
{
    /* The queue arrives as the argument: the task runs the moment it is created,
     * before worker_start() publishes s_job_q (first boot of this build asserted
     * in xQueueReceive on exactly that). */
    QueueHandle_t q = arg;
    for (;;) {
        job_t *job = NULL;
        if (xQueueReceive(q, &job, portMAX_DELAY) != pdTRUE || job == NULL) {
            continue;
        }
        run_job(job);
        HOLD_STEP(hold_on_call_end(&s_hold, now_ms()));
        job_release(job);
    }
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

    if (s_job_q == NULL) {
        cJSON_Delete(args);
        ESP_LOGE(TAG, "concierge worker not running (init failed or not called)");
        reply(session_tag, id, name, spoken_error(CONCIERGE_UNAVAILABLE));
        return;
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

    const size_t text_len = strlen(req->valuestring) + 1;
    char *text = heap_caps_malloc(text_len, MALLOC_CAP_SPIRAM);
    if (text != NULL) {
        memcpy(text, req->valuestring, text_len);
    }
    cJSON_Delete(args);
    if (text == NULL) {
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
    job->queued_us = esp_timer_get_time();

    /* Before the worker can see it, so a fast answer cannot end the call first. */
    HOLD_STEP(hold_on_call_start(&s_hold, now_ms()));
    if (xQueueSend(s_job_q, &job, 0) != pdTRUE) {
        /* Cannot happen with a queue as deep as the job pool; handled anyway. */
        HOLD_STEP(hold_on_call_end(&s_hold, now_ms()));
        reply(session_tag, id, name, spoken_error(CONCIERGE_UNAVAILABLE));
        job_release(job);
    }
}
