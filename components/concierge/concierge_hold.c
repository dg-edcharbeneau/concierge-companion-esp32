/*
 * See concierge_hold.h. Plain C, no allocation: everything here is either the
 * caller's state struct, rodata, or the caller's one text buffer.
 */
#include "concierge_hold.h"

#include <string.h>

/*
 * The pools. Short, plain, and never the same line twice within three picks.
 * Each fits ANNOUNCE_MAX (64 with the NUL) in main/dg_agent.c.
 */
static const char *const SHORT_LINES[] = {
    "One moment.",
    "Let me check on that.",
    "Give me a second, I'm asking the desk.",
    "Just a moment while I check.",
    "Checking with the front desk now.",
    "Hang on, I'll find out.",
    "Let me look into that.",
    "Bear with me a second.",
};
static const char *const LONG_LINES[] = {
    "Still waiting on the desk, thanks for your patience.",
    "The desk is taking a little longer, bear with me.",
    "Almost there, still checking.",
};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/*
 * What "I'm about to check" sounds like. Matched as whole words, ASCII
 * case-insensitive, so "checkout" and "check-in" do not count.
 */
static const char *const PROMISES[] = {
    "check", "checking", "one moment", "a moment", "just a sec", "a second",
    "let me see", "let me find", "let me look", "look into", "look that up",
    "ask the desk", "hang on", "hold on", "bear with me",
};

#define NUDGE_PREFIX "Please ask the desk: "

/* ---------------- helpers ---------------- */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-';
}

static bool phrase_at(const char *text, size_t i, const char *phrase)
{
    size_t n = strlen(phrase);
    for (size_t k = 0; k < n; k++) {
        if (text[i + k] == '\0' || lower(text[i + k]) != phrase[k]) {
            return false;
        }
    }
    const bool start_ok = (i == 0) || !is_word(text[i - 1]);
    const bool end_ok = !is_word(text[i + n]);
    return start_ok && end_ok;
}

bool hold_promises_to_check(const char *text)
{
    if (text == NULL) {
        return false;
    }
    for (size_t i = 0; text[i] != '\0'; i++) {
        for (size_t p = 0; p < COUNT(PROMISES); p++) {
            if (phrase_at(text, i, PROMISES[p])) {
                return true;
            }
        }
    }
    return false;
}

/* Copies at most cap-1 bytes and never ends inside a UTF-8 sequence. */
static void copy_utf8(char *out, size_t cap, const char *in)
{
    size_t n = strlen(in);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)in[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(out, in, n);
    out[n] = '\0';
}

size_t hold_build_nudge(char *out, size_t cap, const char *user_text)
{
    const size_t prefix = strlen(NUDGE_PREFIX);
    if (cap <= prefix + 1) {
        out[0] = '\0';
        return 0;
    }
    memcpy(out, NUDGE_PREFIX, prefix);
    const size_t room = cap - prefix - 1;
    size_t n = strlen(user_text);
    if (n > room) {
        /* Cut at the last space that fits; with none, at a UTF-8 boundary. */
        size_t cut = room;
        while (cut > 0 && user_text[cut] != ' ') {
            cut--;
        }
        if (cut == 0) {
            cut = room;
            while (cut > 0 && ((unsigned char)user_text[cut] & 0xC0) == 0x80) {
                cut--;
            }
        }
        n = cut;
    }
    memcpy(out + prefix, user_text, n);
    out[prefix + n] = '\0';
    return prefix + n;
}

static uint32_t next_rand(hold_state_t *st)
{
    uint32_t x = st->rng ? st->rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    st->rng = x;
    return x;
}

static bool recent(const uint8_t ring[3], uint8_t i)
{
    return ring[0] == i || ring[1] == i || ring[2] == i;
}

static const char *pick(hold_state_t *st, const char *const *pool, size_t count, uint8_t ring[3])
{
    uint8_t candidates[16];
    size_t n = 0;
    for (size_t i = 0; i < count && n < sizeof(candidates); i++) {
        if (!recent(ring, (uint8_t)i)) {
            candidates[n++] = (uint8_t)i;
        }
    }
    /* A pool of three or fewer cannot avoid all of the last three; it then only
     * avoids the very last one. */
    if (n == 0) {
        for (size_t i = 0; i < count; i++) {
            if ((uint8_t)i != ring[0]) {
                candidates[n++] = (uint8_t)i;
            }
        }
    }
    const uint8_t chosen = candidates[next_rand(st) % n];
    ring[2] = ring[1];
    ring[1] = ring[0];
    ring[0] = chosen;
    return pool[chosen];
}

static hold_step_t none(void)
{
    hold_step_t s = { .act = HOLD_ACT_NONE, .text = NULL, .timer_ms = -1,
                      .note = HOLD_NOTE_NONE, .elapsed_ms = 0 };
    return s;
}

static void arm(hold_state_t *st, hold_step_t *s, hold_timer_t what, uint32_t now_ms, uint32_t ms)
{
    st->timer_for = what;
    st->timer_due_ms = now_ms + ms;
    s->timer_ms = (int32_t)(ms > 0 ? ms : 1);
}

static void disarm(hold_state_t *st, hold_step_t *s)
{
    st->timer_for = HOLD_T_NONE;
    s->timer_ms = 0;
}

/* ---------------- events ---------------- */

void hold_init(hold_state_t *st, const hold_config_t *cfg, char *buf, uint32_t seed)
{
    memset(st, 0, sizeof(*st));
    st->cfg = *cfg;
    st->rng = seed;
    st->buf = buf;
    memset(st->recent_short, 0xFF, sizeof(st->recent_short));
    memset(st->recent_long, 0xFF, sizeof(st->recent_long));
    if (buf != NULL) {
        buf[0] = '\0';
        buf[HOLD_TEXT_LEN] = '\0';
    }
}

hold_step_t hold_on_call_start(hold_state_t *st, uint32_t now_ms)
{
    hold_step_t s = none();
    st->turn_called = true;
    /* A call means the guest's turn is over, even if no transcript said so. */
    st->user_speaking = false;
    if (st->calls_pending++ == 0) {
        st->hold_stage = 0;
        st->call_spoke = false;
        st->call_started_ms = now_ms;
        arm(st, &s, HOLD_T_HOLD, now_ms, st->cfg.hold_delay_ms);
    }
    return s;
}

hold_step_t hold_on_call_end(hold_state_t *st, uint32_t now_ms)
{
    hold_step_t s = none();
    if (st->calls_pending == 0) {
        return s;
    }
    if (--st->calls_pending > 0) {
        return s;
    }
    s.elapsed_ms = now_ms - st->call_started_ms;
    if (st->hold_stage == 0 && !st->call_spoke) {
        s.note = HOLD_NOTE_HOLD_SKIPPED;
    }
    if (st->timer_for == HOLD_T_HOLD || st->timer_for == HOLD_T_LONG) {
        disarm(st, &s);
    }
    return s;
}

void hold_on_call_cancel(hold_state_t *st)
{
    st->hold_stage = 2;
}

hold_step_t hold_on_function_call(hold_state_t *st)
{
    hold_step_t s = none();
    st->turn_called = true;
    if (st->timer_for == HOLD_T_RECOVERY) {
        disarm(st, &s);
    }
    return s;
}

bool hold_on_assistant_text(hold_state_t *st, const char *text)
{
    if (text == NULL) {
        return false;
    }
    if (st->echo_pending != NULL && strcmp(text, st->echo_pending) == 0) {
        st->echo_pending = NULL;
        return true;
    }
    if (st->calls_pending > 0) {
        st->call_spoke = true;
    }
    if (hold_promises_to_check(text)) {
        st->turn_intent = true;
    }
    return false;
}

hold_step_t hold_on_user_text(hold_state_t *st, const char *text)
{
    hold_step_t s = none();
    if (text == NULL) {
        return s;
    }
    /* Our own nudge comes back as a user line; it is not the guest speaking. */
    if (st->buf != NULL && st->buf[HOLD_TEXT_LEN] != '\0' &&
        strcmp(text, st->buf + HOLD_TEXT_LEN) == 0) {
        return s;
    }
    if (st->buf != NULL) {
        copy_utf8(st->buf, HOLD_TEXT_LEN, text);
    }
    st->user_speaking = false;
    st->turn_intent = false;
    st->turn_called = false;
    st->recovered = false;
    if (st->timer_for == HOLD_T_RECOVERY) {
        disarm(st, &s);
    }
    return s;
}

hold_step_t hold_on_user_speaking(hold_state_t *st)
{
    hold_step_t s = none();
    st->user_speaking = true;
    if (st->timer_for == HOLD_T_RECOVERY) {
        disarm(st, &s);
    }
    return s;
}

hold_step_t hold_on_agent_done(hold_state_t *st, uint32_t now_ms)
{
    hold_step_t s = none();
    if (st->turn_intent && !st->turn_called && !st->recovered && st->calls_pending == 0 &&
        st->buf != NULL && st->buf[0] != '\0' && st->timer_for == HOLD_T_NONE) {
        arm(st, &s, HOLD_T_RECOVERY, now_ms, st->cfg.recovery_ms);
    }
    return s;
}

hold_step_t hold_on_timer(hold_state_t *st, uint32_t now_ms, bool ready)
{
    hold_step_t s = none();
    const hold_timer_t what = st->timer_for;
    /* A fire for a deadline that has since been replaced or cleared. */
    if (what == HOLD_T_NONE || (int32_t)(st->timer_due_ms - now_ms) > 20) {
        return s;
    }
    st->timer_for = HOLD_T_NONE;
    s.timer_ms = 0;

    switch (what) {
    case HOLD_T_HOLD:
        if (st->calls_pending == 0 || st->hold_stage != 0) {
            break;
        }
        s.elapsed_ms = now_ms - st->call_started_ms;
        if (st->call_spoke) {
            s.note = HOLD_NOTE_HOLD_COVERED;
        } else if (ready && !st->user_speaking) {
            s.act = HOLD_ACT_SAY;
            s.text = pick(st, SHORT_LINES, COUNT(SHORT_LINES), st->recent_short);
            s.note = HOLD_NOTE_HOLD_LINE;
            st->echo_pending = s.text;
        }
        st->hold_stage = 1;
        if (st->cfg.hold_long_ms > st->cfg.hold_delay_ms) {
            arm(st, &s, HOLD_T_LONG, now_ms, st->cfg.hold_long_ms - st->cfg.hold_delay_ms);
        }
        break;

    case HOLD_T_LONG:
        if (st->calls_pending == 0 || st->hold_stage >= 2) {
            break;
        }
        st->hold_stage = 2;
        if (ready && !st->user_speaking) {
            s.act = HOLD_ACT_SAY;
            s.text = pick(st, LONG_LINES, COUNT(LONG_LINES), st->recent_long);
            s.note = HOLD_NOTE_LONG_LINE;
            s.elapsed_ms = now_ms - st->call_started_ms;
            st->echo_pending = s.text;
        }
        break;

    case HOLD_T_RECOVERY:
        if (st->turn_intent && !st->turn_called && !st->recovered && ready &&
            !st->user_speaking && st->calls_pending == 0 && st->buf != NULL &&
            st->buf[0] != '\0') {
            hold_build_nudge(st->buf + HOLD_TEXT_LEN, HOLD_TEXT_LEN, st->buf);
            st->recovered = true;
            s.act = HOLD_ACT_NUDGE;
            s.text = st->buf + HOLD_TEXT_LEN;
            s.note = HOLD_NOTE_RECOVERY;
        }
        break;

    default:
        break;
    }
    return s;
}
