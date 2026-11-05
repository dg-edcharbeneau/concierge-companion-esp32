/*
 * When the device speaks for the desk, and when it rescues a dropped promise.
 *
 * Two jobs, one per-conversation state machine:
 *
 *   HOLD LINES ONLY WHEN THE WAIT NEEDS ONE. A desk call that answers quickly
 *   gets no filler at all; one still pending after hold_delay_ms gets a short,
 *   varied line ("One moment."), and one still pending at hold_long_ms gets a
 *   single "still waiting" line. If the model already said something of its
 *   own since the call started, that counts as the hold. The previous design
 *   spoke the same sentence before every desk answer: robotic, and the model
 *   started copying it -- once saying it without making the call at all.
 *
 *   RECOVERY WHEN THE MODEL PROMISES AND DOES NOT CALL. If an agent turn says
 *   it will check ("let me check", "one moment"...) and no function call of
 *   any kind arrives by recovery_ms after the agent finishes speaking, the
 *   device nudges it with "Please ask the desk: <the guest's last words>".
 *
 * Plain C over libc, no ESP-IDF and no allocation, so host/concierge_hold.sh
 * compiles it on a laptop. Not thread-safe: the caller serialises every call.
 * Phrases are rodata; the only writable buffer is the one the caller passes to
 * hold_init(), so the device can put it in PSRAM.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bytes the caller's buffer must provide: last user text + the nudge built from it. */
#define HOLD_TEXT_LEN  128 /* NUDGE_MAX in main/dg_agent.c: 127 bytes + NUL */
#define HOLD_BUF_LEN   (2 * HOLD_TEXT_LEN)

typedef enum {
    HOLD_ACT_NONE = 0,
    HOLD_ACT_SAY,   /* inject `text` as an agent message (behavior "queue") */
    HOLD_ACT_NUDGE, /* inject `text` as a user message */
} hold_act_t;

/* What happened, for the caller's log line. */
typedef enum {
    HOLD_NOTE_NONE = 0,
    HOLD_NOTE_HOLD_LINE,      /* first-tier line said, after `elapsed_ms` */
    HOLD_NOTE_LONG_LINE,      /* second-tier line said */
    HOLD_NOTE_HOLD_SKIPPED,   /* answered in `elapsed_ms`, no line needed */
    HOLD_NOTE_HOLD_COVERED,   /* due, but the model had already spoken */
    HOLD_NOTE_RECOVERY,       /* promised without a call; nudging */
} hold_note_t;

typedef struct {
    hold_act_t act;
    const char *text;    /* SAY: rodata phrase. NUDGE: inside the caller's buffer. */
    int32_t timer_ms;    /* -1 leave the timer alone, 0 stop it, >0 re-arm for this long */
    hold_note_t note;
    uint32_t elapsed_ms; /* since the call started, for HOLD_* notes */
} hold_step_t;

typedef struct {
    uint32_t hold_delay_ms; /* call pending this long -> first-tier line */
    uint32_t hold_long_ms;  /* still pending this long -> one second-tier line */
    uint32_t recovery_ms;   /* after AgentAudioDone, wait this long for a call */
} hold_config_t;

typedef enum { HOLD_T_NONE = 0, HOLD_T_HOLD, HOLD_T_LONG, HOLD_T_RECOVERY } hold_timer_t;

typedef struct {
    hold_config_t cfg;
    uint32_t rng;

    /* desk call in flight */
    uint8_t calls_pending;
    uint8_t hold_stage;      /* 0 nothing said, 1 first tier said, 2 done for this call */
    bool call_spoke;         /* the model said something since the call started */
    uint32_t call_started_ms;

    /* the current user turn */
    bool user_speaking;
    bool turn_intent;        /* an agent line in this turn promised to check */
    bool turn_called;        /* any function call in this turn */
    bool recovered;          /* one nudge per user turn */

    /* the single timer */
    hold_timer_t timer_for;
    uint32_t timer_due_ms;

    /* device lines awaiting their ConversationText echo */
    const char *echo_pending;

    /* no-repeat memory: last three picks per pool, 0xFF = empty */
    uint8_t recent_short[3];
    uint8_t recent_long[3];

    /* caller's buffer: [0, HOLD_TEXT_LEN) user text, [HOLD_TEXT_LEN, ...) nudge */
    char *buf;
} hold_state_t;

/* `buf` may be NULL: hold lines still work, recovery is disabled. */
void hold_init(hold_state_t *st, const hold_config_t *cfg, char *buf, uint32_t seed);

hold_step_t hold_on_call_start(hold_state_t *st, uint32_t now_ms);
hold_step_t hold_on_call_end(hold_state_t *st, uint32_t now_ms);
/* Barge-in cancelled the pending call: no more filler for it. */
void hold_on_call_cancel(hold_state_t *st);
/* Any FunctionCallRequest entry, ours or the device's own. */
hold_step_t hold_on_function_call(hold_state_t *st);
/* Returns true when `text` is the echo of a line this module asked to say, so the
 * caller keeps it out of the replayed history. */
bool hold_on_assistant_text(hold_state_t *st, const char *text);
hold_step_t hold_on_user_text(hold_state_t *st, const char *text);
hold_step_t hold_on_user_speaking(hold_state_t *st);
hold_step_t hold_on_agent_done(hold_state_t *st, uint32_t now_ms);
/* `ready`: a session is live to say anything into. */
hold_step_t hold_on_timer(hold_state_t *st, uint32_t now_ms, bool ready);

/* Exposed for the host test. */
bool hold_promises_to_check(const char *text);
size_t hold_build_nudge(char *out, size_t cap, const char *user_text);
