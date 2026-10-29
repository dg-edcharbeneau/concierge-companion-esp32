/*
 * Host checks for main/echo.c; run by host/echo.sh.
 */
#include <stdio.h>
#include <string.h>

#include "echo.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        failures++;
    }
}

/* Tick for `span` ms from `*now` in 33 ms frames, as the panel does. Counts
 * the tones sounded and records them in order. A span, not an end time, so a
 * run across the 32-bit wrap still runs. */
static int run(echo_t *g, uint32_t *now, uint32_t span, bool speaking, int *tones, int cap)
{
    int n = 0;
    for (uint32_t t = 0; t < span; t += 33, *now += 33) {
        int tone;
        echo_tick(g, *now, speaking, &tone);
        if (tone >= 0) {
            if (n < cap) {
                tones[n] = tone;
            }
            n++;
        }
    }
    return n;
}

/* Play the round on show back correctly, ending in whatever that leads to. */
static echo_press_t repeat(echo_t *g, uint32_t *now)
{
    echo_press_t r = ECHO_PRESS_IGNORED;
    const int len = g->len;
    for (int i = 0; i < len; i++) {
        *now += 300;
        r = echo_press(g, g->seq[i], *now);
    }
    return r;
}

/* Through SHOW to INPUT for the current round. */
static int show(echo_t *g, uint32_t *now, int *tones, int cap)
{
    const uint32_t span = ECHO_LEAD_MS + (uint32_t)g->len * (ECHO_STEP_MS + ECHO_GAP_MS) + 100;
    return run(g, now, span, false, tones, cap);
}

int main(void)
{
    echo_t g = {0};
    uint32_t now = 1000;
    int tones[ECHO_MAX];

    echo_start(&g, 42, now);
    check("start: level 1, showing", g.state == ECHO_SHOW && echo_level(&g) == 1);

    /* The lead: nothing lit, nothing sounded. */
    int tone;
    check("show: dark through the lead",
          echo_tick(&g, now + ECHO_LEAD_MS - 1, false, &tone) == -1 && tone == -1);
    check("show: first step lit and sounded once the lead is over",
          echo_tick(&g, now + ECHO_LEAD_MS, false, &tone) == g.seq[0] && tone == g.seq[0]);
    check("show: sounded only once while it stays lit",
          echo_tick(&g, now + ECHO_LEAD_MS + 33, false, &tone) == g.seq[0] && tone == -1);
    check("show: dark in the gap after the step",
          echo_tick(&g, now + ECHO_LEAD_MS + ECHO_STEP_MS, false, &tone) == -1);
    check("show: a press during the show is ignored",
          echo_press(&g, g.seq[0], now + ECHO_LEAD_MS + 50) == ECHO_PRESS_IGNORED);

    now += ECHO_LEAD_MS;
    int n = show(&g, &now, tones, ECHO_MAX);
    check("show: hands over to input after the last step", g.state == ECHO_INPUT && n == 0);

    uint8_t first[3];
    memcpy(first, g.seq, 1);
    check("input: the last step completes the round", repeat(&g, &now) == ECHO_PRESS_ROUND);
    check("round: level 2, waiting on the callout", g.state == ECHO_SPEAK && echo_level(&g) == 2);
    check("round: the press stays lit into the wait",
          echo_tick(&g, now + 10, false, &tone) == g.seq[0] && tone == -1);
    check("round: the earlier step is unchanged", g.seq[0] == first[0]);

    /* SPEAK waits for speech to begin and end. */
    run(&g, &now, 1000, false, tones, ECHO_MAX);
    check("speak: still waiting with no speech yet", g.state == ECHO_SPEAK);
    run(&g, &now, 2000, true, tones, ECHO_MAX);
    check("speak: still waiting while it talks, past the timeout", g.state == ECHO_SPEAK);
    run(&g, &now, 33, false, tones, ECHO_MAX);
    check("speak: shows the round once speech ends", g.state == ECHO_SHOW);

    n = show(&g, &now, tones, ECHO_MAX);
    check("show: level 2 sounds both steps, in order",
          n == 2 && tones[0] == g.seq[0] && tones[1] == g.seq[1] && g.state == ECHO_INPUT);

    /* A press right, then wrong. */
    memcpy(first, g.seq, 2);
    check("input: a right press that is not the last", echo_press(&g, g.seq[0], now) == ECHO_PRESS_RIGHT);
    check("input: a wrong press ends the game",
          echo_press(&g, (g.seq[1] + 1) % 4, now + 300) == ECHO_PRESS_WRONG);
    check("over: level and best are the level reached", g.state == ECHO_OVER &&
                                                            echo_level(&g) == 2 && g.best == 2);
    check("over: a wrong press is not a win", !g.won);
    check("over: further presses are ignored", echo_press(&g, 0, now + 600) == ECHO_PRESS_IGNORED);

    /* SPEAK gives up on a callout that never starts. */
    echo_start(&g, 7, now);
    check("restart: best survives, level back to 1", g.best == 2 && echo_level(&g) == 1);
    show(&g, &now, tones, ECHO_MAX);
    repeat(&g, &now);
    run(&g, &now, ECHO_SPEAK_WAIT_MS - 50, false, tones, ECHO_MAX);
    check("speak: waits up to the timeout for silence", g.state == ECHO_SPEAK);
    run(&g, &now, 100, false, tones, ECHO_MAX);
    check("speak: goes on alone past the timeout", g.state == ECHO_SHOW);

    /* Same seed, same game. */
    echo_t a = {0}, b = {0};
    uint32_t ta = 0, tb = 0;
    echo_start(&a, 1234, ta);
    echo_start(&b, 1234, tb);
    for (int r = 0; r < 20; r++) {
        show(&a, &ta, tones, ECHO_MAX);
        repeat(&a, &ta);
        run(&a, &ta, ECHO_SPEAK_WAIT_MS + 100, false, tones, ECHO_MAX);
        show(&b, &tb, tones, ECHO_MAX);
        repeat(&b, &tb);
        run(&b, &tb, ECHO_SPEAK_WAIT_MS + 100, false, tones, ECHO_MAX);
    }
    check("seed: the same seed gives the same 21 steps",
          a.len == 21 && b.len == 21 && memcmp(a.seq, b.seq, 21) == 0);
    int seen[4] = {0};
    for (int i = 0; i < a.len; i++) {
        seen[a.seq[i]]++;
    }
    check("seed: every quarter turns up in 21 steps", seen[0] && seen[1] && seen[2] && seen[3]);

    /* ECHO_MAX repeated wins. */
    echo_t w = {0};
    uint32_t tw = 0;
    echo_start(&w, 99, tw);
    echo_press_t last = ECHO_PRESS_IGNORED;
    while (w.state != ECHO_OVER) {
        show(&w, &tw, tones, ECHO_MAX);
        last = repeat(&w, &tw);
        run(&w, &tw, ECHO_SPEAK_WAIT_MS + 100, false, tones, ECHO_MAX);
    }
    check("max: the last step of ECHO_MAX wins and ends it",
          last == ECHO_PRESS_WON && echo_level(&w) == ECHO_MAX && w.best == ECHO_MAX);
    check("max: a win is marked won", w.won);
    echo_start(&w, 99, tw);
    check("restart: a new game is not won", !w.won);

    /* GO tapped while the agent talks: the lead waits for it. */
    echo_t s = {0};
    uint32_t ts = 5000;
    echo_start(&s, 3, ts);
    n = run(&s, &ts, ECHO_LEAD_MS + 2000, true, tones, ECHO_MAX);
    check("lead: held while the agent talks, nothing sounded", s.state == ECHO_SHOW && n == 0 &&
                                                                   s.pos == 0);
    n = run(&s, &ts, ECHO_LEAD_MS + 33, false, tones, ECHO_MAX);
    check("lead: step 1 sounds once the agent stops", n == 1 && tones[0] == s.seq[0]);
    n = run(&s, &ts, 2000, true, tones, ECHO_MAX);
    check("lead: speech after step 1 holds the round", s.state == ECHO_SHOW && s.pos == 0 &&
                                                          n == 0);
    n = run(&s, &ts, ECHO_LEAD_MS + 33, false, tones, ECHO_MAX);
    check("lead: and replays step 1 once it stops", n == 1 && tones[0] == s.seq[0]);
    run(&s, &ts, ECHO_STEP_MS + ECHO_GAP_MS, false, tones, ECHO_MAX);
    check("lead: then hands over to input", s.state == ECHO_INPUT);

    /* The panel over a round mid-sequence, then gone. */
    echo_t r = {0};
    uint32_t tr = 0;
    echo_start(&r, 11, tr);
    for (int i = 0; i < 3; i++) {
        show(&r, &tr, tones, ECHO_MAX);
        repeat(&r, &tr);
        run(&r, &tr, ECHO_SPEAK_WAIT_MS + 100, false, tones, ECHO_MAX);
    }
    run(&r, &tr, ECHO_LEAD_MS + ECHO_STEP_MS + ECHO_GAP_MS + 50, false, tones, ECHO_MAX);
    check("resume: level 4 part way through its show", r.state == ECHO_SHOW && r.len == 4 &&
                                                           r.pos == 2);
    tr += 10000;   /* not ticked: the panel is up */
    echo_resume(&r, tr);
    check("resume: still showing, from the lead", r.state == ECHO_SHOW &&
                                                      echo_tick(&r, tr + 1, false, &tone) == -1);
    n = show(&r, &tr, tones, ECHO_MAX);
    check("resume: all four steps play again, in order",
          n == 4 && memcmp((int[]){r.seq[0], r.seq[1], r.seq[2], r.seq[3]}, tones,
                           4 * sizeof(int)) == 0 && r.state == ECHO_INPUT);
    check("resume: the player's turn is left alone",
          (echo_resume(&r, tr + 5000), r.state == ECHO_INPUT && r.pos == 0));

    repeat(&r, &tr);
    run(&r, &tr, ECHO_SPEAK_WAIT_MS - 1000, false, tones, ECHO_MAX);
    echo_resume(&r, tr);
    run(&r, &tr, ECHO_SPEAK_WAIT_MS - 100, false, tones, ECHO_MAX);
    check("resume: the callout's wait starts over", r.state == ECHO_SPEAK);
    run(&r, &tr, 200, false, tones, ECHO_MAX);
    check("resume: and still times out", r.state == ECHO_SHOW);

    /* Started just before the 32-bit ms wrap. */
    echo_t z = {0};
    uint32_t tz = 0xFFFFFF00u;
    echo_start(&z, 5, tz);
    show(&z, &tz, tones, ECHO_MAX);
    check("wrap: plays a round across the ms wrap", z.state == ECHO_INPUT);

    printf("\n%s\n", failures ? "FAILURES" : "all ok");
    return failures ? 1 : 0;
}
