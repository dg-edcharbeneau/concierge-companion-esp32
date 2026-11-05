#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "concierge.h"

/* Base64url of 32 random bytes is 43 characters; generous for format changes. */
#define CONCIERGE_CREDENTIAL_LEN 129

typedef struct {
    char base_url[CONCIERGE_URL_LEN];
    char last_name[CONCIERGE_LAST_NAME_LEN];
    char room[CONCIERGE_ROOM_LEN];
} concierge_guest_t;

/* NVS first, Kconfig seed second. False when any field is still empty. */
bool concierge_store_load_guest(concierge_guest_t *out);

/*
 * What a desk turn needs, loaded from NVS ONCE at init and then only touched by
 * the concierge worker. The worker's stack is in PSRAM, and a PSRAM-stack task
 * must not do flash operations (NVS reads and writes included), so nothing on
 * the turn path reads or writes NVS. The portal saves settings and reboots, so
 * a load at boot is never stale. The credential lives in RAM only: a reboot
 * costs one extra POST /sessions on the first question.
 */
typedef struct {
    concierge_guest_t guest;
    bool configured;
    char credential[CONCIERGE_CREDENTIAL_LEN]; /* "" until verified */
} concierge_session_t;

typedef enum {
    CONCIERGE_OK = 0,
    CONCIERGE_NOT_CONFIGURED, /* no URL / guest saved */
    CONCIERGE_UNREACHABLE,    /* DNS, TLS, connect or timeout */
    CONCIERGE_GUEST_NOT_FOUND,/* 404 guest_not_found from /sessions */
    CONCIERGE_UNAUTHORIZED,   /* 401 after re-verifying: stay ended or reset */
    CONCIERGE_REJECTED,       /* 400 / 409: the request itself was refused */
    CONCIERGE_UNAVAILABLE,    /* 5xx */
    CONCIERGE_BAD_RESPONSE,   /* 2xx without the expected JSON */
} concierge_status_t;

/*
 * One concierge turn: verifies first when no credential is cached, and once
 * more on a 401 (credentials die with the stay or a reset). The same turn_id is
 * reused on that retry so the stay workflow joins rather than duplicates it.
 * On CONCIERGE_OK, *answer is heap-allocated and owned by the caller.
 */
concierge_status_t concierge_client_ask(concierge_session_t *session, const char *turn_id,
                                        const char *text, char **answer);
