/*
 * HTTPS calls to the concierge's public API (Concierge.Web ConciergeApiEndpoints):
 *
 *   POST /api/concierge/sessions  {lastName, roomNumber}            -> {credential, ...}
 *   POST /api/concierge/turns     {turnId, userMessage, channel}    -> {assistantMessage, ...}
 *                                 Authorization: Bearer <credential>
 *
 * ASP.NET's web JSON defaults read property names case-insensitively and write
 * camelCase, so camelCase is used both ways. Errors are {code, message}.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "concierge_internal.h"

static const char *TAG = "concierge";

/* Answers are one or two spoken sentences; this only has to bound a bad reply. */
#define RESPONSE_MAX (8 * 1024)

#ifndef CONFIG_CONCIERGE_TURN_TIMEOUT_MS
#define CONFIG_CONCIERGE_TURN_TIMEOUT_MS 25000
#endif

typedef struct {
    int status;    /* HTTP status, or -1 when no response arrived */
    char *body;    /* PSRAM, NUL-terminated, may be NULL */
} http_result_t;

static void http_result_free(http_result_t *r)
{
    free(r->body);
    r->body = NULL;
}

/*
 * ONE CONNECTION, KEPT OPEN BETWEEN TURNS. A fresh TLS handshake per question
 * cost the device ~1-1.6 s over the same request from a laptop (bench,
 * 2026-11-04: 3.3-4.8 s on the device against 2.5-3.2 s from the Mac). Only the
 * concierge worker uses this, one turn at a time (s_run_lock), so it needs no
 * lock of its own.
 */
static esp_http_client_handle_t s_http;
static char s_http_origin[CONCIERGE_URL_LEN];
static int64_t s_http_last_us;
/* Close rather than reuse a connection idle this long: a front end that has
 * already dropped it turns the next turn into a failed write plus a retry. */
#define IDLE_CLOSE_US (45LL * 1000 * 1000)

typedef struct {
    char *buf;
    int len;
    char server[48]; /* Server response header: tells the tunnel from Kestrel */
} body_sink_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    body_sink_t *sink = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && sink != NULL &&
        evt->header_key != NULL && strcasecmp(evt->header_key, "Server") == 0) {
        strlcpy(sink->server, evt->header_value ? evt->header_value : "", sizeof(sink->server));
    }
    if (evt->event_id == HTTP_EVENT_ON_DATA && sink != NULL && sink->buf != NULL) {
        int room = RESPONSE_MAX - sink->len;
        int n = evt->data_len < room ? evt->data_len : room;
        if (n > 0) {
            memcpy(sink->buf + sink->len, evt->data, n);
            sink->len += n;
        }
    }
    return ESP_OK;
}

static void http_drop(void)
{
    if (s_http != NULL) {
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
    s_http_origin[0] = '\0';
}

static esp_http_client_handle_t http_get(const char *origin, const char *url)
{
    const int64_t now = esp_timer_get_time();
    if (s_http != NULL &&
        (strcmp(origin, s_http_origin) != 0 || now - s_http_last_us > IDLE_CLOSE_US)) {
        http_drop();
    }
    if (s_http == NULL) {
        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = CONFIG_CONCIERGE_TURN_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
            .keep_alive_enable = true,
            .event_handler = on_http_event,
        };
        s_http = esp_http_client_init(&cfg);
        if (s_http == NULL) {
            return NULL;
        }
        strlcpy(s_http_origin, origin, sizeof(s_http_origin));
        esp_http_client_set_header(s_http, "Accept", "application/json");
        /* Dev tunnels interpose a warning page for browser-like clients on
         * anonymous tunnels; this opts out explicitly. */
        esp_http_client_set_header(s_http, "X-Tunnel-Skip-AntiPhishing-Page", "true");
    } else {
        esp_http_client_set_url(s_http, url);
        esp_http_client_set_method(s_http, HTTP_METHOD_POST);
    }
    return s_http;
}

static esp_err_t post_once(const char *origin, const char *url, const char *auth,
                           const char *json, body_sink_t *sink, int *status)
{
    esp_http_client_handle_t c = http_get(origin, url);
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (auth != NULL) {
        esp_http_client_set_header(c, "Authorization", auth);
    } else {
        esp_http_client_delete_header(c, "Authorization");
    }
    esp_http_client_set_user_data(c, sink);
    /*
     * EVERY REQUEST, NOT ONCE AT INIT. esp_http_client_set_post_field(NULL) below
     * also deletes Content-Type, and with none set the next post field defaults
     * it to application/x-www-form-urlencoded. ASP.NET then treats the request as
     * a form post and refuses it for want of an antiforgery token: every request
     * after the first on a kept connection failed that way on the bench.
     */
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, json, (int)strlen(json));
    sink->len = 0;
    sink->server[0] = '\0';
    esp_err_t err = esp_http_client_perform(c);
    /* The post field points at the caller's buffer; never leave it dangling. */
    esp_http_client_set_post_field(c, NULL, 0);
    if (err == ESP_OK) {
        *status = esp_http_client_get_status_code(c);
        s_http_last_us = esp_timer_get_time();
    } else {
        http_drop();
    }
    return err;
}

static void post_json(const char *origin, const char *url, const char *bearer,
                      const char *json, http_result_t *out)
{
    out->status = -1;
    out->body = NULL;

    char *auth = NULL;
    if (bearer != NULL) {
        size_t n = strlen("Bearer ") + strlen(bearer) + 1;
        auth = malloc(n);
        if (auth == NULL) {
            return;
        }
        snprintf(auth, n, "Bearer %s", bearer);
    }
    body_sink_t sink = { .buf = heap_caps_malloc(RESPONSE_MAX + 1, MALLOC_CAP_SPIRAM) };

    const bool reused = (s_http != NULL);
    int status = -1;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = post_once(origin, url, auth, json, &sink, &status);
    /* Only a FAST failure on a kept connection is a stale socket; a slow one is
     * the desk timing out, and resending would just wait the timeout again. */
    if (err != ESP_OK && reused && esp_timer_get_time() - t0 < 3LL * 1000 * 1000) {
        /* Safe to resend: a turn with the same turnId joins the one already
         * admitted, and an extra verify only mints a spare session. */
        ESP_LOGI(TAG, "kept connection failed (%s); retrying on a new one", esp_err_to_name(err));
        err = post_once(origin, url, auth, json, &sink, &status);
    } else if (reused && status == 400 &&
               (sink.buf == NULL || memmem(sink.buf, sink.len, "\"code\"", 6) == NULL)) {
        /*
         * A 400 the concierge API did not write (its errors always carry a
         * code) on a kept connection: seen on the bench 2026-11-04 for every
         * request after the second on one socket, while the same body on a fresh
         * connection succeeds. Log what answered, then resend on a new socket;
         * the turnId makes that safe.
         */
        if (sink.buf != NULL) {
            sink.buf[sink.len < 160 ? sink.len : 160] = '\0';
        }
        ESP_LOGW(TAG, "kept connection got a non-API 400 (server \"%s\", body \"%s\"); retrying on a new one",
                 sink.server, sink.buf ? sink.buf : "");
        http_drop();
        err = post_once(origin, url, auth, json, &sink, &status);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST %s failed: %s", url, esp_err_to_name(err));
        free(sink.buf);
    } else {
        out->status = status;
        if (sink.buf != NULL) {
            sink.buf[sink.len] = '\0';
            out->body = sink.buf;
        }
    }
    if (auth != NULL) {
        memset(auth, 0, strlen(auth));
        free(auth);
    }
}

static const char *json_string(const cJSON *root, const char *name)
{
    const cJSON *v = cJSON_GetObjectItem(root, name); /* case-insensitive on purpose */
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void log_api_error(const char *what, const http_result_t *r)
{
    cJSON *root = r->body ? cJSON_Parse(r->body) : NULL;
    const char *code = root ? json_string(root, "code") : NULL;
    const char *message = root ? json_string(root, "message") : NULL;
    ESP_LOGW(TAG, "%s -> HTTP %d [%s] %s", what, r->status, code ? code : "?",
             message ? message : "");
    cJSON_Delete(root);
}

static concierge_status_t verify(const concierge_guest_t *g, char *credential, size_t cap)
{
    char url[CONCIERGE_URL_LEN + 32];
    snprintf(url, sizeof(url), "%s/api/concierge/sessions", g->base_url);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "lastName", g->last_name);
    cJSON_AddStringToObject(req, "roomNumber", g->room);
    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (json == NULL) {
        return CONCIERGE_UNAVAILABLE;
    }

    http_result_t r;
    post_json(g->base_url, url, NULL, json, &r);
    cJSON_free(json);

    concierge_status_t st;
    if (r.status < 0) {
        st = CONCIERGE_UNREACHABLE;
    } else if (r.status == 200) {
        cJSON *root = r.body ? cJSON_Parse(r.body) : NULL;
        const char *c = root ? json_string(root, "credential") : NULL;
        if (c != NULL && strlen(c) < cap) {
            strlcpy(credential, c, cap);
            concierge_store_save_credential(c);
            ESP_LOGI(TAG, "guest verified");
            st = CONCIERGE_OK;
        } else {
            st = CONCIERGE_BAD_RESPONSE;
        }
        cJSON_Delete(root);
    } else {
        log_api_error("verify", &r);
        st = (r.status == 404) ? CONCIERGE_GUEST_NOT_FOUND
           : (r.status >= 500) ? CONCIERGE_UNAVAILABLE
                               : CONCIERGE_REJECTED;
    }
    http_result_free(&r);
    return st;
}

static concierge_status_t submit(const concierge_guest_t *g, const char *credential,
                                 const char *turn_id, const char *text, char **answer)
{
    char url[CONCIERGE_URL_LEN + 32];
    snprintf(url, sizeof(url), "%s/api/concierge/turns", g->base_url);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "turnId", turn_id);
    cJSON_AddStringToObject(req, "userMessage", text);
    /* "voice" selects the concierge's spoken answer style (no lists or links). */
    cJSON_AddStringToObject(req, "channel", "voice");
    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (json == NULL) {
        return CONCIERGE_UNAVAILABLE;
    }

    http_result_t r;
    post_json(g->base_url, url, credential, json, &r);
    cJSON_free(json);

    concierge_status_t st;
    if (r.status < 0) {
        st = CONCIERGE_UNREACHABLE;
    } else if (r.status == 200) {
        cJSON *root = r.body ? cJSON_Parse(r.body) : NULL;
        const char *a = root ? json_string(root, "assistantMessage") : NULL;
        if (a != NULL) {
            *answer = strdup(a);
            st = (*answer != NULL) ? CONCIERGE_OK : CONCIERGE_UNAVAILABLE;
        } else {
            st = CONCIERGE_BAD_RESPONSE;
        }
        cJSON_Delete(root);
    } else {
        log_api_error("turn", &r);
        st = (r.status == 401) ? CONCIERGE_UNAUTHORIZED
           : (r.status >= 500) ? CONCIERGE_UNAVAILABLE
                               : CONCIERGE_REJECTED;
    }
    http_result_free(&r);
    return st;
}

concierge_status_t concierge_client_ask(const char *turn_id, const char *text, char **answer)
{
    *answer = NULL;
    concierge_guest_t g;
    if (!concierge_store_load_guest(&g)) {
        return CONCIERGE_NOT_CONFIGURED;
    }

    char credential[CONCIERGE_CREDENTIAL_LEN];
    bool fresh = false;
    if (!concierge_store_load_credential(credential, sizeof(credential))) {
        concierge_status_t v = verify(&g, credential, sizeof(credential));
        if (v != CONCIERGE_OK) {
            return v;
        }
        fresh = true;
    }

    concierge_status_t st = submit(&g, credential, turn_id, text, answer);
    if (st == CONCIERGE_UNAUTHORIZED && !fresh) {
        /* Sessions end with the stay or a reset; one re-verify covers both. */
        concierge_store_clear_credential();
        concierge_status_t v = verify(&g, credential, sizeof(credential));
        if (v != CONCIERGE_OK) {
            return v;
        }
        st = submit(&g, credential, turn_id, text, answer);
    }
    memset(credential, 0, sizeof(credential));
    return st;
}
