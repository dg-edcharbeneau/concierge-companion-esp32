/* See echo.h. */
#include "echo.h"

/* xorshift32: four quarters need nothing better, and the same seed has to give
 * the same game on a laptop as on the board. */
static uint8_t next_step(echo_t *g)
{
    uint32_t x = g->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng = x;
    return (uint8_t)(x >> 30);
}

static void enter(echo_t *g, echo_state_t s, uint32_t now_ms)
{
    g->state = s;
    g->since_ms = now_ms;
    g->pos = 0;
    g->spoke = false;
}

static void game_over(echo_t *g, uint32_t now_ms)
{
    if (g->len > g->best) {
        g->best = g->len;
    }
    enter(g, ECHO_OVER, now_ms);
}

void echo_start(echo_t *g, uint32_t seed, uint32_t now_ms)
{
    const uint16_t best = g->best;
    *g = (echo_t){.best = best, .press_k = -1, .rng = seed ? seed : 0x9E3779B9u};
    g->seq[0] = next_step(g);
    g->len = 1;
    enter(g, ECHO_SHOW, now_ms);
}

int echo_tick(echo_t *g, uint32_t now_ms, bool speaking, int *tone)
{
    *tone = -1;
    const uint32_t in = now_ms - g->since_ms;

    switch (g->state) {
    case ECHO_SPEAK:
        if (speaking) {
            g->spoke = true;
        } else if (g->spoke || in >= ECHO_SPEAK_WAIT_MS) {
            enter(g, ECHO_SHOW, now_ms);
        }
        break;

    case ECHO_SHOW: {
        if (speaking) {
            /* Agent audio silences tones, so nothing shown under it counts:
             * the round plays again, lead and all, once it stops. */
            g->since_ms = now_ms;
            g->pos = 0;
            break;
        }
        if (in < ECHO_LEAD_MS) {
            break;
        }
        const uint32_t t = in - ECHO_LEAD_MS;
        const uint32_t i = t / (ECHO_STEP_MS + ECHO_GAP_MS);
        if (i >= g->len) {
            enter(g, ECHO_INPUT, now_ms);
            g->press_k = -1;
            break;
        }
        const bool on = t % (ECHO_STEP_MS + ECHO_GAP_MS) < ECHO_STEP_MS;
        /* Sounded once, on the first tick that sees it lit. A step a slow frame
         * stepped over entirely is skipped rather than sounded late. */
        if (g->pos <= i) {
            if (on) {
                *tone = g->seq[i];
            }
            g->pos = (uint8_t)(i + 1);
        }
        return on ? g->seq[i] : -1;
    }

    default:
        break;
    }

    if (g->press_k >= 0 && now_ms - g->press_ms < ECHO_PRESS_MS) {
        return g->press_k;
    }
    return -1;
}

void echo_resume(echo_t *g, uint32_t now_ms)
{
    switch (g->state) {
    case ECHO_SHOW:
        enter(g, ECHO_SHOW, now_ms);
        break;
    case ECHO_SPEAK:
        g->since_ms = now_ms;   /* `spoke` kept: a callout heard is heard */
        break;
    default:
        break;
    }
    g->press_k = -1;
}

echo_press_t echo_press(echo_t *g, int k, uint32_t now_ms)
{
    if (g->state != ECHO_INPUT || k < 0 || k > 3) {
        return ECHO_PRESS_IGNORED;
    }
    g->press_k = (int8_t)k;
    g->press_ms = now_ms;
    if (g->seq[g->pos] != k) {
        game_over(g, now_ms);
        return ECHO_PRESS_WRONG;
    }
    if (++g->pos < g->len) {
        return ECHO_PRESS_RIGHT;
    }
    if (g->len >= ECHO_MAX) {
        game_over(g, now_ms);
        g->won = true;
        return ECHO_PRESS_WON;
    }
    g->seq[g->len] = next_step(g);
    g->len++;
    enter(g, ECHO_SPEAK, now_ms);
    return ECHO_PRESS_ROUND;
}
