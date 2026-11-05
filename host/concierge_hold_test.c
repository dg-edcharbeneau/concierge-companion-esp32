/*
 * Exercises components/concierge/concierge_hold.c: when the device fills a desk
 * wait, when it stays quiet, and when it rescues a promise the model did not
 * keep. Plain C, compiled as is; times are made up and passed in.
 */
#include <stdio.h>
#include <string.h>

#include "concierge_hold.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("%-64s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        failures++;
    }
}

static const hold_config_t CFG = { .hold_delay_ms = 1500, .hold_long_ms = 8000, .recovery_ms = 1200 };
static char buf[HOLD_BUF_LEN];

static void fresh(hold_state_t *st)
{
    hold_init(st, &CFG, buf, 12345);
}

int main(void)
{
    hold_state_t st;
    hold_step_t s;

    /* Fast answer: no line at all. */
    fresh(&st);
    hold_on_user_text(&st, "What time does the pool open?");
    s = hold_on_call_start(&st, 1000);
    check("call start arms the hold timer", s.timer_ms == 1500 && st.timer_for == HOLD_T_HOLD);
    s = hold_on_call_end(&st, 2200);
    check("fast answer: skipped, timer stopped", s.note == HOLD_NOTE_HOLD_SKIPPED && s.timer_ms == 0);
    check("fast answer: elapsed reported", s.elapsed_ms == 1200);
    s = hold_on_timer(&st, 2500, true);
    check("stale timer after a fast answer says nothing", s.act == HOLD_ACT_NONE);

    /* Slow answer: one first-tier line, then the long timer. */
    fresh(&st);
    hold_on_user_text(&st, "Where is the nearest coffee shop?");
    hold_on_call_start(&st, 0);
    s = hold_on_timer(&st, 1500, true);
    check("slow answer: one hold line", s.act == HOLD_ACT_SAY && s.note == HOLD_NOTE_HOLD_LINE && s.text);
    check("slow answer: long timer armed for the remainder", s.timer_ms == 6500 && st.timer_for == HOLD_T_LONG);
    check("hold line echo is recognised and kept out of history", hold_on_assistant_text(&st, s.text));
    check("the echo does not count as the model speaking", !st.call_spoke);
    s = hold_on_call_end(&st, 3000);
    check("answer after a hold line: no skip note, timer stopped", s.note == HOLD_NOTE_NONE && s.timer_ms == 0);

    /* Very slow: the second tier, once. */
    fresh(&st);
    hold_on_user_text(&st, "Book me a ride to the airport.");
    hold_on_call_start(&st, 0);
    hold_on_timer(&st, 1500, true);
    s = hold_on_timer(&st, 8000, true);
    check("very slow: one still-waiting line", s.act == HOLD_ACT_SAY && s.note == HOLD_NOTE_LONG_LINE);
    s = hold_on_timer(&st, 9000, true);
    check("no third line", s.act == HOLD_ACT_NONE);

    /* The model's own filler covers the hold. */
    fresh(&st);
    hold_on_user_text(&st, "What's the weather tomorrow?");
    hold_on_call_start(&st, 0);
    check("model filler is not a device line", !hold_on_assistant_text(&st, "Sure, let me look."));
    s = hold_on_timer(&st, 1500, true);
    check("model already spoke: no device line, noted as covered",
          s.act == HOLD_ACT_NONE && s.note == HOLD_NOTE_HOLD_COVERED);

    /* No session or a talking guest: stay quiet. */
    fresh(&st);
    hold_on_call_start(&st, 0);
    s = hold_on_timer(&st, 1500, false);
    check("no live session: nothing said", s.act == HOLD_ACT_NONE);
    fresh(&st);
    hold_on_call_start(&st, 0);
    hold_on_user_speaking(&st);
    s = hold_on_timer(&st, 1500, true);
    check("guest talking: nothing said", s.act == HOLD_ACT_NONE);

    /* Barge-in cancels the call: no filler for it. */
    fresh(&st);
    hold_on_call_start(&st, 0);
    hold_on_call_cancel(&st);
    s = hold_on_timer(&st, 1500, true);
    check("cancelled call: no hold line", s.act == HOLD_ACT_NONE);

    /* No repeats within three picks, across many calls. */
    fresh(&st);
    const char *last[3] = { 0 };
    int repeats = 0;
    for (int i = 0; i < 200; i++) {
        uint32_t t = (uint32_t)i * 20000;
        hold_on_user_text(&st, "question");
        hold_on_call_start(&st, t);
        s = hold_on_timer(&st, t + 1500, true);
        if (s.text && (s.text == last[0] || s.text == last[1] || s.text == last[2])) {
            repeats++;
        }
        hold_on_assistant_text(&st, s.text);
        last[2] = last[1];
        last[1] = last[0];
        last[0] = s.text;
        hold_on_call_end(&st, t + 3000);
    }
    check("200 slow calls: no line repeats within three", repeats == 0);

    /* Intent detection. */
    check("'Let me check on that.' promises", hold_promises_to_check("Let me check on that."));
    check("'One moment, let me check with the desk.' promises",
          hold_promises_to_check("One moment, let me check with the desk."));
    check("'Checkout is at 11 a.m.' does not", !hold_promises_to_check("Checkout is at 11 a.m."));
    check("'Check-in starts at 3.' does not", !hold_promises_to_check("Check-in starts at 3."));
    check("'The pool opens at 8.' does not", !hold_promises_to_check("The pool opens at 8."));

    /* Recovery: promised, no call. */
    fresh(&st);
    hold_on_user_text(&st, "Where is the nearest coffee shop?");
    hold_on_assistant_text(&st, "One moment, let me check with the desk.");
    s = hold_on_agent_done(&st, 10000);
    check("promise without a call arms recovery", s.timer_ms == 1200 && st.timer_for == HOLD_T_RECOVERY);
    s = hold_on_timer(&st, 11200, true);
    check("recovery nudges with the guest's words",
          s.act == HOLD_ACT_NUDGE && strcmp(s.text, "Please ask the desk: Where is the nearest coffee shop?") == 0);
    s = hold_on_user_text(&st, "Please ask the desk: Where is the nearest coffee shop?");
    check("the nudge's own echo is not a new guest turn", st.recovered && strcmp(buf, "Where is the nearest coffee shop?") == 0);
    hold_on_assistant_text(&st, "Let me check.");
    s = hold_on_agent_done(&st, 13000);
    check("only one nudge per guest turn", s.timer_ms == -1 && st.timer_for == HOLD_T_NONE);

    /* Recovery: promised, but a function call came. */
    fresh(&st);
    hold_on_user_text(&st, "What time is it?");
    hold_on_assistant_text(&st, "Let me check the time.");
    hold_on_function_call(&st);
    s = hold_on_agent_done(&st, 10000);
    check("promise with get_time: no recovery", s.timer_ms == -1);
    fresh(&st);
    hold_on_user_text(&st, "Where is the nearest coffee shop?");
    hold_on_assistant_text(&st, "Let me check.");
    hold_on_agent_done(&st, 10000);
    s = hold_on_function_call(&st);
    check("a call inside the window cancels recovery", s.timer_ms == 0 && st.timer_for == HOLD_T_NONE);

    /* Recovery: barge-in cancels. */
    fresh(&st);
    hold_on_user_text(&st, "Where is the nearest coffee shop?");
    hold_on_assistant_text(&st, "Hang on.");
    hold_on_agent_done(&st, 10000);
    s = hold_on_user_speaking(&st);
    check("guest speaking cancels recovery", s.timer_ms == 0 && st.timer_for == HOLD_T_NONE);

    /* Recovery never fires on the device's own line. */
    fresh(&st);
    hold_on_user_text(&st, "question");
    hold_on_call_start(&st, 0);
    s = hold_on_timer(&st, 1500, true);
    hold_on_assistant_text(&st, s.text);
    hold_on_call_end(&st, 2000);
    s = hold_on_agent_done(&st, 2500);
    check("a device hold line never triggers recovery", s.timer_ms == -1);

    /* Nudge length and truncation. */
    char out[HOLD_TEXT_LEN];
    char longq[400];
    memset(longq, 0, sizeof(longq));
    for (int i = 0; i < 60; i++) {
        strcat(longq, "word ");
    }
    size_t n = hold_build_nudge(out, sizeof(out), longq);
    check("nudge stays within 127 bytes", n <= 127 && strlen(out) == n);
    check("nudge is cut at a word boundary", out[n - 1] == 'd');
    char nospace[300];
    memset(nospace, 0, sizeof(nospace));
    for (int i = 0; i < 100; i++) {
        strcat(nospace, "\xc3\xa9"); /* é, two bytes, no spaces */
    }
    n = hold_build_nudge(out, sizeof(out), nospace);
    check("nudge without spaces stays within 127 bytes", n <= 127);
    check("nudge without spaces never splits UTF-8", ((unsigned char)out[n - 1] & 0xC0) != 0xC0 &&
                                                       ((n - strlen("Please ask the desk: ")) % 2) == 0);

    /* No buffer (PSRAM allocation failed): hold lines still work, recovery off. */
    hold_init(&st, &CFG, NULL, 1);
    hold_on_call_start(&st, 0);
    s = hold_on_timer(&st, 1500, true);
    check("no buffer: hold line still said", s.act == HOLD_ACT_SAY);
    hold_on_call_end(&st, 2000);
    hold_on_user_text(&st, "question");
    hold_on_assistant_text(&st, "Let me check.");
    s = hold_on_agent_done(&st, 3000);
    check("no buffer: recovery disabled", s.timer_ms == -1);

    printf("\nsizeof(hold_state_t) = %zu bytes\n", sizeof(hold_state_t));
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
