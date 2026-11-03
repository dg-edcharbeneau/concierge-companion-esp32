/*
 * Thin on purpose: each control calls what the voice tool or the BOOT button
 * already calls, minus the flash write. See controls.h.
 */
#include "controls.h"

#include <stdio.h>

#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "audio_io.h"
#include "clock.h"
#include "dg_agent.h"
#include "echo.h"
#include "session_ctl.h"

static const char *TAG = "controls";

#define VOLUME_STEP 10   /* the voice tools' "small change" */

/* LVGL task only: the panel is the only caller. */
static bool s_unsaved;
static uint32_t s_changed_ms;

static void changed(void)
{
    s_unsaved = true;
    s_changed_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

int controls_volume(void)
{
    return audio_io_get_volume();
}

int controls_volume_step(int dir)
{
    /* Snap to the step grid, so a level set by voice (say 65) moves to 70 or 60
     * rather than walking 75, 85 for ever off the marks the panel shows. */
    const int now = audio_io_get_volume();
    int want = (dir > 0) ? ((now / VOLUME_STEP) + 1) * VOLUME_STEP
                         : ((now + VOLUME_STEP - 1) / VOLUME_STEP - 1) * VOLUME_STEP;
    const int level = audio_io_set_volume_live(want);
    if (level != now) {
        changed();
        ESP_LOGI(TAG, "EVT ctl volume %d", level);
    }
    return level;
}

bool controls_mic_gain_available(void)
{
#if CONFIG_MIC_GAIN_TOOL
    return true;
#else
    return false;
#endif
}

int controls_mic_gain(void)
{
    return audio_io_get_mic_gain();
}

int controls_mic_gain_step(int dir)
{
    const int now = audio_io_get_mic_gain();
#if CONFIG_MIC_GAIN_TOOL
    const int g = audio_io_set_mic_gain_live(now + (dir > 0 ? 1 : -1) * AUDIO_MIC_GAIN_STEP_DB);
    if (g != now) {
        changed();
        ESP_LOGI(TAG, "EVT ctl micgain %d", g);
    }
    return g;
#else
    (void)dir;
    return now;
#endif
}

bool controls_ns_available(void)
{
#if CONFIG_MIC_NS_ENABLE
    return true;
#else
    return false;
#endif
}

bool controls_ns_on(void)
{
#if CONFIG_MIC_NS_ENABLE
    return audio_io_ns_enabled();
#else
    return false;
#endif
}

void controls_ns_toggle(void)
{
#if CONFIG_MIC_NS_ENABLE
    /* Exactly the BOOT double-click: set now, save on the worker. */
    const bool on = !audio_io_ns_enabled();
    audio_io_ns_set(on);
    session_ctl_request_ns_save();
    ESP_LOGI(TAG, "EVT ctl ns %s", on ? "on" : "off");
#endif
}

bool controls_clock_available(void)
{
#if CONFIG_CLOCK
    return true;
#else
    return false;
#endif
}

bool controls_clock_12h(void)
{
    return clock_12h();
}

void controls_clock_toggle_12h(void)
{
    const bool h12 = !clock_12h();
    clock_set_12h(h12);   /* clock_tick() applies and saves it */
    ESP_LOGI(TAG, "EVT ctl clock %d-hour", h12 ? 12 : 24);
}

int controls_alarms(alarm_view_t *out, int max)
{
    return alarms_snapshot(out, max);
}

bool controls_alarm_cancel(uint8_t id)
{
    const bool ok = alarms_cancel_id(id);
    ESP_LOGI(TAG, "EVT ctl cancel id=%u %s", id, ok ? "done" : "gone");
    return ok;
}

/* ---------------- the stopwatch ---------------- */

static sw_t s_sw;
static portMUX_TYPE s_sw_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t ms_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void controls_stopwatch_get(sw_t *out)
{
    portENTER_CRITICAL(&s_sw_lock);
    *out = s_sw;
    portEXIT_CRITICAL(&s_sw_lock);
}

void controls_stopwatch_toggle(void)
{
    const uint32_t now = ms_now();
    portENTER_CRITICAL(&s_sw_lock);
    sw_toggle(&s_sw, now);
    const sw_state_t st = s_sw.state;
    portEXIT_CRITICAL(&s_sw_lock);
    ESP_LOGI(TAG, "EVT ctl stopwatch %s", st == SW_RUNNING ? "run" : "pause");
}

bool controls_stopwatch_lap(void)
{
    const uint32_t now = ms_now();
    portENTER_CRITICAL(&s_sw_lock);
    const bool ok = sw_lap(&s_sw, now);
    const unsigned n = s_sw.laps;
    portEXIT_CRITICAL(&s_sw_lock);
    if (ok) {
        ESP_LOGI(TAG, "EVT ctl stopwatch lap %u", n);
    }
    return ok;
}

bool controls_stopwatch_reset(void)
{
    portENTER_CRITICAL(&s_sw_lock);
    const bool ok = sw_reset(&s_sw);
    portEXIT_CRITICAL(&s_sw_lock);
    if (ok) {
        ESP_LOGI(TAG, "EVT ctl stopwatch reset");
    }
    return ok;
}

void controls_alarm_stop(void)
{
    ESP_LOGI(TAG, "EVT ctl alarm stop");
    alarms_dismiss();
}

void controls_alarm_snooze(int minutes)
{
    ESP_LOGI(TAG, "EVT ctl alarm snooze %d", minutes);
    alarms_snooze(minutes);
}

void controls_echo_open(bool open)
{
    ESP_LOGI(TAG, "EVT ctl echo %s", open ? "open" : "close");
    if (!open) {
        /* Before the history hold goes: a callout for a game that is over is
         * not worth saying, and one still queued would land unheld. */
        dg_agent_nudge_cancel();
    }
    audio_io_uplink_hold(AUDIO_HOLD_ECHO, open);
    dg_agent_history_hold(open);
    /* A flag, not a start, as an alarm asks: a no-op on a session that is up,
     * and the start that lands once a stopping one has finished. */
    if (open && !session_ctl_is_running()) {
        session_ctl_request_start_soon();
    }
}

/*
 * A SHAPE FOR EACH CALLOUT. Asked the same way every level, the agent copies its
 * own last callout: against the Agent API with the device's prompt, levels 2 to
 * 17 all came back "Level N, nice, ..." -- "you're rolling now", "good
 * momentum". A persona alone ("like mission control") changed the words but
 * not the frame, "Echo level N... quip", and "don't open with the number" was
 * ignored by level 6. Naming where the number goes, or how the line opens,
 * breaks the frame. Dealt from a shuffled deck, so none comes round again
 * until all have.
 */
static const char *const CALLOUT_SHAPES[] = {
    "that builds up and lands on the number last",
    "that opens with a sound like whoa or ooh",
    "as a mock news headline",
    "like a sports commentator",
    "as dry understatement, the number at the end",
    "that opens with a silly title for them",
    "with a pun on the number",
    "like a weather report",
    "that dares them to beat the next round",
    "like a game-show host",
};
#define CALLOUT_SHAPES_N (sizeof(CALLOUT_SHAPES) / sizeof(CALLOUT_SHAPES[0]))

static const char *deal_shape(void)
{
    static uint8_t deck[CALLOUT_SHAPES_N];
    static size_t left;
    if (left == 0) {
        for (size_t i = 0; i < CALLOUT_SHAPES_N; i++) {
            deck[i] = (uint8_t)i;
        }
        for (size_t i = CALLOUT_SHAPES_N - 1; i > 0; i--) {
            const size_t j = esp_random() % (i + 1);
            const uint8_t t = deck[i];
            deck[i] = deck[j];
            deck[j] = t;
        }
        left = CALLOUT_SHAPES_N;
    }
    return CALLOUT_SHAPES[deck[--left]];
}

void controls_echo_callout(int level)
{
    /*
     * The agent writes the line; this says what it is for. In English whatever
     * the voice: the prompt's language line already tells it what to answer
     * in. "No questions" because a question invites a turn, and the player's
     * microphone is held. The longest is 105 B, inside the nudge's 127.
     */
    const char *tone = level < 10 ? "encouraging" : level < 15 ? "teasing" : "snarky";
    char say[128];
    snprintf(say, sizeof(say), "[Echo] Reached %d. One short, %s line %s. No questions.", level,
             tone, deal_shape());
    dg_agent_nudge(say);
}

void controls_echo_won(void)
{
    ESP_LOGI(TAG, "EVT ctl echo won");
    char say[96];
    snprintf(say, sizeof(say),
             "[Echo] I just beat all %d levels. Celebrate in one short sentence. No questions.",
             ECHO_MAX);
    dg_agent_nudge(say);
}

void controls_commit(uint32_t now_ms, uint32_t quiet_ms)
{
    /* Signed, for the same reason as the panel's idle timer: a change stamped
     * after the frame that calls this began is "in the future", not ancient. */
    if (s_unsaved && (int32_t)(now_ms - s_changed_ms) >= (int32_t)quiet_ms) {
        s_unsaved = false;
        session_ctl_request_settings_save();
    }
}
