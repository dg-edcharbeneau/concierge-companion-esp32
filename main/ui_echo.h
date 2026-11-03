#pragma once

/*
 * The Echo screen: Ed's "Echo" design. Four quarter buttons in the colour
 * palette's Phosphor, Clay, Brass and Glass, dim until lit, round a hub that reads
 * GO before a game and the level during one -- its border in the orb's colour
 * while it is the player's turn. Game over -- or "You win", past ECHO_MAX --
 * puts the level and the best above a half-circle Play again.
 *
 * Between levels the agent calls the new one out (controls_echo_callout()),
 * and the next round waits for it to finish. While the screen is up the
 * microphone is held, the conversation is kept out of the history, and a
 * session is started if none is running (controls_echo_open()).
 *
 * The rules are echo.c's; this paints them, sounds them and takes the presses.
 * ui.c decides when it shows and hands it the frame and the presses, as it does
 * the ring screen. LVGL task only, except where a function says otherwise.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

void ui_echo_init(lv_obj_t *canvas);

/* Echo is wanted: a fresh game on GO, and the holds taken. */
void ui_echo_enter(uint32_t now_ms);

/* Echo is no longer wanted: the game is abandoned and the holds let go. */
void ui_echo_close(void);

/* Something else owns the frame now -- the panel over it, or Echo closed.
 * `blank` blacks the canvas for the face; the next frame here repaints whole,
 * and replays a sequence that was part way through (echo_resume()). */
void ui_echo_leave(bool blank);

/* Paint one frame, and run the game to `now_ms`. */
void ui_echo_frame(uint32_t now_ms, uint32_t tint_rgb);

/* A press while the screen is up. One on nothing does nothing. */
void ui_echo_pressed(int32_t x, int32_t y, uint32_t now_ms);

/* Whether someone is playing: a press or a round in the last two minutes. Any
 * task; one 32-bit read. */
bool ui_echo_recent(uint32_t now_ms);

/* The best level reached, for main.c to save when it changes. Any task. */
uint16_t ui_echo_best(void);

/* Write `best` to NVS. A flash write: main.c's loop, never the LVGL task. */
void ui_echo_best_persist(uint16_t best);
