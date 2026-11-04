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

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
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

static void post_json(const char *url, const char *bearer, const char *json, http_result_t *out)
{
    out->status = -1;
    out->body = NULL;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_CONCIERGE_TURN_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    /* Dev tunnels interpose a warning page for browser-like clients on anonymous
     * tunnels; this opts out explicitly instead of relying on the User-Agent. */
    esp_http_client_set_header(client, "X-Tunnel-Skip-AntiPhishing-Page", "true");

    char *auth = NULL;
    if (bearer != NULL) {
        size_t n = strlen("Bearer ") + strlen(bearer) + 1;
        auth = malloc(n);
        if (auth == NULL) {
            esp_http_client_cleanup(client);
            return;
        }
        snprintf(auth, n, "Bearer %s", bearer);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    const int len = (int)strlen(json);
    esp_err_t err = esp_http_client_open(client, len);
    if (err == ESP_OK && esp_http_client_write(client, json, len) != len) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST %s failed: %s", url, esp_err_to_name(err));
        goto done;
    }

    out->status = esp_http_client_get_status_code(client);
    char *body = heap_caps_malloc(RESPONSE_MAX + 1, MALLOC_CAP_SPIRAM);
    if (body != NULL) {
        int got = 0;
        /* Read until EOF rather than trusting Content-Length: a tunnel may
         * re-frame the response as chunked. */
        while (got < RESPONSE_MAX) {
            int r = esp_http_client_read(client, body + got, RESPONSE_MAX - got);
            if (r <= 0) {
                break;
            }
            got += r;
        }
        body[got] = '\0';
        out->body = body;
    }

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
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
    post_json(url, NULL, json, &r);
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
    post_json(url, credential, json, &r);
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
