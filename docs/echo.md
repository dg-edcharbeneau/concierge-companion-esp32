# Echo

A memory game on the panel, after Ed's "Echo" design. Four quarter buttons
in the palette's Phosphor, Clay, Brass and Glass (green, red, yellow and
blue) stay dim until lit. They sit
round a hub that reads **GO / TAP TO START** before a game and the level during
one. Each round plays the sequence, you repeat it, and a right repeat adds one
step. A wrong press ends the game, with the level and the best above a
half-circle **Play again**.

Between levels **the agent calls out the new level**, and the next round waits
until it has finished. The agent writes each line itself, and its tone changes
with the level:

| level | tone | heard on the board |
| --- | --- | --- |
| 1-9 | encouraging | "Level three, nice work, you've got it." |
| 10-14 | teasing | "Level twelve already, alright, somebody's getting a little too smooth at this." |
| 15 and up | snarky | "Level seventeen, wow, look at you acting like the pattern owes you money." |

## Getting there

- **Swipe:** sideways on the stopwatch screen. The swipe cycle is orb clock →
  stopwatch → Echo → orb clock. Swipe up from the bottom edge to get the clock
  first.
- **Voice:** "let's play Echo" calls `play_echo`, and Echo comes up while the
  conversation carries on, waking the panel if it slept. If the screen is taken
  (an alarm, the setup QR code, the display test, the battery-critical sleep),
  the reply tells the model so, and it says Echo can't open now.

A sideways swipe leaves it, and abandons the game. An alarm ringing, the setup
QR code, the display test or the panel going to sleep also close it. A session
starting does not.

## What it does to the conversation

Echo needs an agent to call the levels, so while it is up:

- **A session is started if none is running.** The request is the same flag an
  alarm uses (`session_ctl_request_start_soon()`). A fresh conversation says its
  greeting first. The game does not wait for it: GO works at once.
- **The microphone is held** (`audio_io_uplink_hold(AUDIO_HOLD_ECHO, …)`). The
  agent hears neither the tones nor the player, and talking to it mid-game does
  nothing. The hold is one bit of several: an alarm's hold is its own, so Echo
  letting go on its way out does not let go of the alarm's.
- **The conversation is kept out of the history** (`dg_agent_history_hold()`).
  The callouts are neither stored nor replayed into the next session, and the
  `[Echo]` lines do not count as user turns. A callout already sent when Echo
  closes stays out too, with its reply, for up to 10 s (`NUDGE_TAIL_MS`); one
  not yet sent is dropped, as it is when the session ends.
- **The session's idle timeout is suspended** while someone is playing (a press
  or a round in the last two minutes). The held microphone means nobody speaks,
  and the timeout would otherwise end the session mid-game.

Leaving Echo lets the hold and the history go. BOOT still toggles the session;
with it stopped, the game carries on without callouts.

## How a callout works

At each level up, `controls_echo_callout()` hands the agent a line as if the
user had said it: `InjectUserMessage`, through `dg_agent_nudge()`. For example:

> [Echo] I just reached level 12. Call out the level in one short, teasing
> sentence. No questions.

The instruction travels in the message, so the system prompt and `Settings`
carry nothing for it; the tool itself adds 113 B to `Settings`. The request is in
English whatever the voice: the prompt's language line already tells the agent
what to answer in. "No questions" because a question invites a turn, and the
player's microphone is held.

A nudge is only for the session that is up **now**. Unlike an alarm's
announcement it is never the greeting and never retried. A nudge made with no
session ready is dropped (`EVT nudge dropped (no session)`); one refused by the
server is logged (`EVT nudge refused`) and dropped.

**The round waits for the reply.** Agent audio takes the speaker ahead of any
tone, so a sequence played under it would be silent. After a level up the game
sits in `ECHO_SPEAK` until `audio_io_playback_active()` has gone true and then
false. If speech has not started within 5 s, because there is no session or the
nudge was refused, it goes on without. On the board the pause from the last
right press to the next sequence was 3.5-4.4 s. Opened by swipe on a cold device,
the first callout or two can be dropped while the session is still connecting.

## Tones

Each quarter has its own note, a G major arpeggio: green G4 (392 Hz), red C5
(523), yellow E5 (659), blue G5 (784). A wrong press gets 150 Hz for 600 ms.
`audio_io_tone()` asks the playback task to synthesise them, exactly as it does
the alarm chime. Nothing is queued into the playback ring, and agent audio and
the chime both outrank a tone.

A tone does **not** count as playback for `audio_io_playback_active()`. That
function goes on meaning "the agent is speaking", which is what the wait above
needs: the player's own last press would otherwise read as the callout. Tones
keep their own write stamp, which only the amplifier's idle timer reads.

## The design, and where it differs

- **Painted like the ring screen.** The quarters and the half-circle are the
  touch panel's precomputed shapes (`ui_panel_kit_fill`), so what is lit is
  exactly what can be pressed. The screen is repainted only when something
  changes: a quarter lighting, the level, the turn, the orb colour. A quarter
  going on or off, two to every step, repaints only its own box.
- **Dim is about a third** of each colour (`ui_tone_mul(0x525252, …)`), which
  matches the design's dark shades.
- **The hub** is 140 px across. Its ring is grey, and turns the orb colour while
  it is your turn. During play it shows the level over a spaced `LEVEL`.
- **It takes the orb colour** for its text, the hub ring and Play again, as the
  ring screen does. On a white orb it is the design exactly; the quarters are
  always those four palette colours.
- **The icon on Play again** is a full-size version of the stopwatch's reset
  arrow (`UI_ICON_REPLAY`).
- **Presses act on the press**, as on the ring and stopwatch screens. A press
  stays a candidate swipe, because the quarters cover nearly the whole screen.
  A swipe out abandons the game, so the press it began with does not matter.
  The exception is the top 40 px: a press there may be the drag that opens the
  settings panel, so it is not a move.

## Rules and timing

`main/echo.c` is the game with no LVGL or IDF, checked on a laptop by
`host/echo.sh`. Each step is lit for 420 ms with 180 ms dark between, after a
600 ms lead; the timing is the same at every level. A press lights its quarter
for 250 ms. There is no input timeout. The game ends at a wrong press, or after
99 steps repeated: a win, which sounds the top quarter's note instead of the
buzz, reads "You win", and has the agent celebrate.

A round waits in its lead while the agent is talking, so GO tapped during a
reply or a greeting does not play steps the agent's audio would silence. Speech
that starts part way through a round, such as the reply to "let's play Echo"
landing just after GO, or a callout that came late, plays the round again from
its lead once it stops. The
settings panel opened over the game pauses it; a sequence it covered plays again
from its lead once the panel closes.

**Level** is the number of steps in the round being played, or in the round that
ended the game. **Best** is the highest level reached. It is kept in NVS
(`dgagent/echo_best`), written by the main loop 2 s after a game ends, not on
the LVGL task. A flash write stalls audio, and the end of a game is the
wrong-press tone.

## Power

While someone is playing, the panel stays awake and the session's idle timeout is
suspended. Two minutes after the last press both lapse: the session times out as
usual, the panel sleeps 30 s later, and sleep closes Echo. Tones wake the
speaker amplifier like any other sound, and it goes off 5 s after the last one.

## Kconfig

| option | default | |
| --- | --- | --- |
| `CONFIG_ECHO` | y | the game, its tool and its swipe; needs `CONFIG_CLOCK` |
| `CONFIG_UI_ECHO_SELFTEST` | n | bench only: see below |

`CONFIG_UI_ECHO_SELFTEST` opens Echo 10 s after boot. It plays two rounds
right and a wrong press at level 3 through the real hit test. It prints captures
of GO, a step lit, the wait for the callout, your turn and game over for
`host/shot2png.py`. Then it asks for the level 12 and 17 callouts, so the teasing
and snarky replies are in the log.

---

[Back to the README](../README.md)
