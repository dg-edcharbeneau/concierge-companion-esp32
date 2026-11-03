#pragma once

/*
 * Echo, as arithmetic: the sequence, whose turn it is, which quarter is lit
 * and when. Plain C with the time passed in, so host/echo.sh drives it on a
 * laptop; ui_echo.c keeps the device's one game and draws it.
 *
 * A ROUND: the sequence plays (SHOW), the player repeats it (INPUT), and a
 * repeat that comes out right adds one step. Between rounds the game waits
 * for the agent to call out the new level (SPEAK) -- agent audio takes the
 * speaker ahead of any tone, so a sequence played under it would be silent.
 *
 * Quarters are numbered as the screen lays them out: 0 top left, 1 top right,
 * 2 bottom left, 3 bottom right. Milliseconds are the esp_timer's, truncated
 * to 32 bits; differences are taken unsigned.
 */

#include <stdbool.h>
#include <stdint.h>

#define ECHO_MAX           99     /* steps; the 99th repeated wins */
#define ECHO_STEP_MS       420    /* a step lit, in SHOW */
#define ECHO_GAP_MS        180    /* dark between steps */
#define ECHO_LEAD_MS       600    /* dark before a round plays */
#define ECHO_PRESS_MS      250    /* a press lit, in INPUT */
/* SPEAK gives up on a callout that has not begun by then: no session, a
 * refused inject, or a reply that never came. The game goes on in silence. */
#define ECHO_SPEAK_WAIT_MS 5000

typedef enum {
    ECHO_READY = 0,   /* GO, not started */
    ECHO_SPEAK,       /* waiting out the level callout */
    ECHO_SHOW,        /* the sequence plays */
    ECHO_INPUT,       /* the player repeats it */
    ECHO_OVER,        /* a wrong press, or ECHO_MAX repeated */
} echo_state_t;

typedef enum {
    ECHO_PRESS_IGNORED = 0,   /* not the player's turn */
    ECHO_PRESS_RIGHT,         /* the next step, and more to go */
    ECHO_PRESS_ROUND,         /* the last step: a level up, now in SPEAK */
    ECHO_PRESS_WRONG,         /* game over */
    ECHO_PRESS_WON,           /* the last step of ECHO_MAX: game over */
} echo_press_t;

typedef struct {
    echo_state_t state;
    uint8_t seq[ECHO_MAX];
    uint8_t len;          /* steps this round: the level */
    uint8_t pos;          /* SHOW: next step to sound; INPUT: next step expected */
    uint16_t best;        /* highest level reached; the caller loads and saves it */
    uint32_t since_ms;    /* when the current phase began */
    uint32_t rng;
    bool spoke;           /* SPEAK: the callout has started */
    bool won;             /* OVER: ECHO_MAX repeated, not a wrong press */
    int8_t press_k;       /* the last quarter pressed, lit for ECHO_PRESS_MS */
    uint32_t press_ms;
} echo_t;

/* A new game, level 1, its first step about to play. `best` is kept. */
void echo_start(echo_t *g, uint32_t seed, uint32_t now_ms);

/*
 * Advance to `now_ms`. `speaking` is whether the agent is talking. Returns the
 * quarter lit now, or -1. `*tone` is set to the quarter whose step began on
 * this tick -- the caller sounds it -- and to -1 otherwise.
 *
 * While the agent talks a round holds in its lead -- the reply to "let's
 * play", a greeting, a callout that came late -- for the reason SPEAK waits;
 * speech that starts part way through a round plays the round again, from
 * its lead, once it stops.
 */
int echo_tick(echo_t *g, uint32_t now_ms, bool speaking, int *tone);

/*
 * The game was not ticked for a while -- the panel over it -- and is about to
 * be again. A sequence that was playing plays again from its lead: the steps
 * missed are steps the player never saw. A wait for the callout starts over;
 * the player's turn is left as it was.
 */
void echo_resume(echo_t *g, uint32_t now_ms);

/* The player pressed quarter `k` (0..3). See echo_press_t. */
echo_press_t echo_press(echo_t *g, int k, uint32_t now_ms);

/* The level: steps in the round being played, or the round that ended it. */
static inline int echo_level(const echo_t *g)
{
    return g->len;
}
