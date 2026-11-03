# Changing things by asking

Everything the agent can change about itself mid-conversation -- volume, name,
face, orb colour, voice -- and what each one costs, plus what it can read about
itself: charge, Wi-Fi signal, and the time. Timers and alarms are on their own
page.

Volume, mic gain, orb colour, the clock format and cancelling a timer or alarm
can also be done by touch, from the panel a swipe down from the top edge opens;
see [the touch panel](display.md#the-touch-panel). Both routes call the same
code.

## Changing the volume by asking

There are two tools, because "turn it down a bit" and "set your volume to 50" are
different requests and one signed delta cannot serve both:

- `adjust_volume` takes a signed `delta` — relative, both directions.
- `set_volume` takes an absolute `level`.

Both funnel through `audio_io_set_volume()`, so the clamp, the register write and
the NVS save happen in exactly one place. The level persists across reboots under
the same `dgagent` NVS namespace as the voice.

The scale runs **20 to 100**, and 20 is the floor rather than 0 on purpose:
`esp_codec_dev` maps volume 0 to -96 dB, which is silence, and an agent that has
muted itself cannot be asked to unmute. There is no mute. Asking for less than 20
lands on 20, and the function response tells the agent to say so out loud rather
than silently ignoring the number it was given.

It is a much simpler feature than the voice change, for one reason: **Deepgram
knows nothing about the volume.** It is a single ES8311 register write, absent
from the `Settings` message entirely — so there is no session reload, no history
replay and no gap. The confirmation sentence is itself spoken at the new level,
which is the feedback.

### The microphone gain, for tuning (experimental)

`set_mic_gain` takes a `db`: "set your microphone gain to 21". It is a bench tool
behind `CONFIG_MIC_GAIN_TOOL`, for re-measuring `CONFIG_MIC_IN_GAIN` without a
rebuild. `audio_io_set_mic_gain()` rounds to the ES7210's 3 dB steps within
6..30, saves it, and leaves the codec call to the capture task, which owns the
microphone. That task sets the device-wide gain, puts the echo-reference lane
back to `CONFIG_AEC_REF_GAIN_DB`, and rescales the uplink gate so it keeps
meaning the same loudness; the log line is `EVT micgain 21 dB, gate 283`. 18 dB
is the measured value -- see [tuning.md](tuning.md).

### Scale — and why the stock curve had to go

`esp_codec_dev`'s default curve maps 0-100 onto **-50..0 dB**. On this speaker
that wastes most of the control. Measured on the actual hardware:

| stock volume | effective dB | audible result |
|---|---|---|
| 20-60 | -36 to -16 | essentially silent |
| 70 | -11 | hardly audible |
| 80 | -6 | low |
| 90 | -1 | medium |
| 100 | +4 | max |

**Sixty of the hundred steps did nothing**, and the entire usable range was
crammed into the top thirty. The audible floor sits around -12 to -16 dB
effective; the stock curve spends more than half its travel below that.

So the curve is now replaced unconditionally — not just when adding gain — with
one spanning `CONFIG_AUDIO_OUT_RANGE_DB` (default **20 dB**) below the top:

| volume | effective dB | equivalent to old volume |
|---|---|---|
| 20 | -12 | 68 |
| 40 | -8 | 76 |
| 60 | -4 | 84 |
| 80 | 0 | 92 |
| 100 | +4 | 100 |

Maximum is unchanged; the whole 20-100 travel now lands inside the band you can
actually hear, and each 10 points is 2 dB — a real, roughly even step. Linear in
dB is the perceptually even axis, so equal numeric steps sound like equal
changes.

The range is expressed as **dB below the top** rather than as an absolute floor,
so raising `AUDIO_OUT_EXTRA_GAIN_DB` shifts the whole scale up and preserves the
travel instead of stretching it.

The runtime floor is 20 rather than 0 on purpose: `esp_codec_dev` special-cases
volume 0 to **-96 dB** whatever the curve says, which is silence rather than
quiet — and **an agent that has muted itself cannot be asked to unmute.** Muting,
if it is ever wanted, needs a route back that is not the voice.

The boot line reports the resolved level and the dB it asked the curve for:

```
audio_io: codecs open: 16000 Hz, 16-bit, 2 ch | volume 50 (saved) = -10 dB (range 20 dB, gain +0 dB), mic gain 24 dB
```

`CONFIG_AUDIO_OUT_VOLUME` is the factory default, same arrangement as the voice.

### Calling the codec from the WebSocket task is safe here

`esp_codec_dev_set_out_vol()` runs on the WebSocket task while `audio_play` is
inside `esp_codec_dev_write()` on the *same handle*, and `esp_codec_dev` has no
lock of any kind. That is still safe, but the reason is board-specific and worth
recording:

- The two paths touch disjoint peripherals — `write()` is **I2S**, `set_out_vol()`
  is one **I2C** register write (`ES8311_DAC_REG32`, a plain write, unlike
  `set_mute` which is read-modify-write).
- The one field both read is `dev->sw_vol`, and it is **NULL on this board**
  because the ES8311 supplies a hardware volume. That is the crux: the
  software-volume path, which *would* mutate the PCM buffer underneath the
  writer, is never taken. On a codec without hardware volume this would race.
- The I2C driver is itself thread-safe — a bus-wide lock with a bounded 100 ms
  timeout, which also serialises against touch polling on the same bus.
- Cost is ~300-500 us. The codec is registered at 100 kHz; the 400 kHz
  `CONFIG_BSP_I2C_CLK_SPEED_HZ` applies only to the touch panel.

If a touch or UI volume control is ever added, funnel both callers through
`audio_io_set_volume()` and put a mutex there — outside `esp_codec_dev`, so it
cannot invert against the WebSocket client's own recursive lock.

## Asking how much charge is left

`get_battery` is one of the two tools here that change nothing -- they read. "How much
charge is left?", "are you plugged in?", "how long will you last?" all land on
it, and it answers with a percentage the agent says out loud.

The number comes from the AXP2101's own fuel gauge, sampled every 5 s by
`main/battery.c`, not from a voltage the firmware guesses a curve for. The tool
description tells the model it runs on a battery and that this is the only way
it can know the level, because otherwise it has no way to know either fact and
will happily invent a number.

Four answers, and the differences are deliberate:

| state | what it says |
|---|---|
| charging | "at 62 percent and charging" |
| done, still plugged in | "at 62 percent and done charging, still plugged in" |
| discharging | "at 62 percent, not plugged in" |
| low (<= `CONFIG_BATTERY_LOW_PCT`) | the percentage, plus a suggestion to plug in |
| no reading | says it cannot read the battery, and is told not to guess |

"Charging" comes from the AXP2101's charge **state machine** -- trickle,
pre-charge, constant-current, constant-voltage are all still charging -- and not
from its current-direction field. The direction field falls back to standby as
the charge tapers into constant-voltage, so anything keyed to it stops saying
"charging" while the charger is still working. It is also gated on VBUS, so it
can never claim charging on a device running off the cell.

"Done" is its own answer rather than falling into "not charging", which would
also be true on battery and reads as a fault. Note that the charger stops at a
**configured target voltage** (`REG64[2:0]`, one of 4.0/4.1/4.2/4.35/4.4 V,
logged once at startup), and a target below the cell's rating means "done"
arrives well short of 100% on the gauge -- so the percentage is worth saying
alongside it rather than being rounded up to "full".

The dots on screen use the same reading, and the same hysteresed `low` flag, so
what the device says and what it shows cannot disagree. See
[the display](display.md) for where they are drawn.

## Asking the time, and setting timers and alarms

Four tools, all described in [the clock page](clock.md): `get_time`,
`set_clock` (timezone and 12/24-hour), `set_alarm` (timers too) and `alarms`
(list and cancel).
"What time is it?", "I'm in Chicago", "set a pasta timer for ten minutes",
"wake me at 7 on weekdays", "what alarms do I have?" and "cancel all of them"
all land on one of them.

The model is told plainly that it does not know the time and must read it every
time. It will otherwise answer from the last time it read the clock, or from
nothing at all. Without a valid clock, `get_time` says so and `set_alarm`
refuses, but a timer still works, on the monotonic clock.

## Playing Echo by asking

"Let's play Echo" calls `play_echo`, which brings [the game](echo.md) up
while the conversation carries on. From then on the agent calls out each new
level: encouraging to level 9, teasing to 14, snarky past that. Those calls are
not tools. The device puts each one to the agent as a user message
(`InjectUserMessage`) with its instruction inside, so the prompt carries nothing
for them; the tool adds 113 B to `Settings`. The microphone is held while the
game is up, so talking to the agent means swiping out of it first.

## Asking how the Wi-Fi is

`get_signal_strength` is the other reading tool. "How's your wifi?", "how's your
connection?" and "why do you keep cutting out?" all land on it.

`esp_wifi_sta_get_ap_info()` returns the RSSI the Wi-Fi driver already has from
the beacons it receives, so this costs no scan and no radio time -- unlike the
scan in `main/wifi_prov.c`, which starves the link for hundreds of ms and only
ever runs while there is no session. There is no sampling task; the number is
read where it is used.

**The dBm never reaches the model.** "Minus sixty-two dBm" spoken aloud tells the
listener nothing, and a number with no scale attached invites the model to invent
one, so `main/wifi_sta.c` buckets it and `dg_agent.c` turns the bucket into the
sentence:

| bars | dBm | what it says |
|---|---|---|
| 4 | >= -55 | "excellent" |
| 3 | >= -67 | "good" |
| 2 | >= -75 | "fair" |
| 1 | >= -85 | "weak", and that it may cut out |
| 0 | < -85 | "very weak", plus a suggestion to move closer to the router |
| no association | -- | says it cannot read the signal, and is told not to guess |

Buckets are **hysteresed by 3 dB on the way up only**: beacon RSSI walks several
dB between beacons, so a device sitting at a boundary would otherwise flicker
between two answers. Losing a bar is reported at once, because that is the
reading worth acting on; gaining one waits for margin.

There is a standing irony here worth knowing about when reading a log: a link bad
enough to matter may never deliver the tool call at all. What this tool describes
is always a link that was good enough to carry the question.

The wifi icon on screen uses the same bucket and the same hysteresis, so what
the device says and what it shows cannot disagree. See
[the display](display.md) for how it is drawn. Independently of the tool, the
driver's own RSSI threshold (`CONFIG_WIFI_SIGNAL_WEAK_DBM`, default -80 dBm) logs
**one** line per excursion below it -- which is what puts a cause in the log
immediately before the dropped audio that follows.

One line, not one per crossing, and the difference was measured: the driver's
threshold is one-shot, so the handler re-arms it, and re-arming while the link is
still below it makes it fire again about once a second. A ten-second dip produced
thirteen identical lines and buried the TLM output at the moment it was worth
reading. So the arming is unconditional and the *line* is edge-triggered -- gated
until `wifi_sta_get_signal()` sees the reading recover past the threshold plus the
same 3 dB, which makes the 1 Hz reader the thing that decides an excursion has
ended.

Two limits worth knowing when reading a log. The driver's own averaging is
coarser than the 1 Hz sample, so **not every dip visible in `rssi=` raises an
event** -- measured dips to -80 dBm passed without one. And the 3 dB recovery
margin can swallow a second crossing if the link only recovers part-way. Both are
acceptable because `rssi=` is on the TLM line every second regardless: the
warning is a convenience for spotting a cause quickly, the TLM line is the
record.

## Changing its name by asking

The agent is called **Grammer** out of the box. "Call yourself Blake" changes
that, and it survives a reboot; "go back to your old name" undoes it.

Precedence is the voice's rule exactly: `CONFIG_AGENT_NAME` is a factory default
used on first boot and after an NVS erase, and once someone has renamed it the
saved name wins on every later boot. `reset_name` is what brings the Kconfig
value back.

Mechanically it is a third pattern, and the reason is worth keeping. `set_voice`
reloads the session because the voice is a `Settings` field this account will not
change in place. `set_face` does not, because Deepgram has never heard of the
display. The name is neither: it is **text inside the system prompt**, reaching
the model as `{{name}}` in [main/prompt/identity.md](../main/prompt/identity.md). So
the function response is enough for the session in progress, and `{{name}}`
carries it from the next `Settings` onward — no reload, no history replay, and
the two agree again the moment anything reconnects.

There is no catalog, because any name is valid. That puts the whole burden on the
function description, which is written against what actually goes wrong: the name
arrives through speech-to-text, so the model is repeating something it *heard*,
and left alone it hands over the entire sentence it heard it in.

[main/agent_name.c](../main/agent_name.c) trims and validates before storing.
Control characters are refused outright — a newline in a "name" would forge a
heading in the assembled prompt — and the 31-character cap is doing two jobs: a
name is spoken aloud, and this is a string a stranger says out loud that ends up
in the system prompt.

## Starting over by asking

"Let's start a new conversation" works, and it is the only function here that
destroys something. `new_conversation` takes no arguments and forgets everything
the device is holding, in RAM and on flash.

Mechanically it is a fourth pattern, because it is the only one that has to
defend against being called. The description tells the model to ask first — but a
description is a request, so **the confirmation is enforced in code**: the first
call arms and does nothing, and only a second call counts — inside sixty seconds,
and after **exactly one** user turn.

That buys three things: the device asked before it wiped, somebody spoke after
being asked, and it was recent. It does **not** verify that the answer was yes —
"no" is a user turn like any other and nothing on the device can read it. What
the scheme removes is the failure where nobody was asked at all. "Exactly one"
rather than "at least one" additionally means an unanswered question from earlier
in the minute cannot be cashed in later; the cost is that a confirmation split
over two utterances does not count and the device asks again, which is the
direction to be wrong in.

Like `set_voice` it reloads the session, because the turns are gone from the
device but Deepgram still holds them server-side. Unlike `set_voice` the reload
carries a backstop timer, because this is the one deferral that cannot afford
`AgentAudioDone` not arriving — see [persistence.md](persistence.md).

The screen has the same gesture for a device whose session is down: hold, and
hold again. [session-control.md](session-control.md#holding-on-a-stopped-device)
covers why the confirmation there is a second hold rather than a tap.

## Changing the face by asking

"Show me the bars instead" works, and so does "go back to the orb". `set_face`
takes a name from a two-entry catalog.

Mechanically it is the sibling of `adjust_volume`, not of `set_voice`:
**Deepgram knows nothing about the display.** No `Settings` field, so no session
reload, no history replay and no gap — the confirmation is phrased as already
done, because it is.

The catalog lives in [main/faces.c](../main/faces.c) with no LVGL in it, mirroring
the `voices.c` / `voices.h` split for exactly the same reason: `dg_agent` has to
build the function schema, and its blurbs are written for the model rather than
for a reader, so indirect phrasings resolve without anyone naming a face
exactly. `ui_set_face()` stores an index that the frame timer applies **before**
anything draws, so the incoming face owns a whole frame instead of painting over
half of the outgoing one's output.

## Changing the orb's colour by asking

"Make it purple" works, and so does "put it back to normal". `set_color` takes a
name from the seven-colour palette in [main/orb_colors.c](../main/orb_colors.c),
from Deepgram's brand palette: Fuchsia, the primary and the default, then Clay,
Bakelite, Brass, Phosphor, Glass and Led. The model gets each brand name with its
plain colour word ("led (purple)"), so "make it purple" lands on Led. The tool is
the sibling of `set_face` in most mechanical respects: no `Settings` field,
no session reload, applied by the frame timer. Unlike the face it **survives a
reboot**: `main.c`'s loop saves it by name to NVS once a change has held for 2 s
(so trying swatches on the touch panel costs one flash write, not one a tap), and
the device boots in it.

**The orb, and the screen around it.** The status word, the sleep clock, the
hints, the ring round the button and the touch panel all take the colour, by the
rules in [main/ui_tone.h](../main/ui_tone.h) — see the touch panel section of
[display.md](display.md). On a white orb every one of them would be exactly as
designed; the palette has no white, so every colour re-hues them.
**Not the spectrum**: it colours its bands by frequency and by which half of the
conversation is live, so a single tint would destroy information rather than
restyle it. The function description says so, which is what stops the model
offering a colour change when someone complains about the bars. The QR code stays
black on white too, because that is what a phone can scan, and the amber "tap:
interrupt" ring stays amber, because its job is to look different.

Colour is one multiply per channel on the ink the geometry already resolved, at
the single point in [main/orb_raster.c](../main/orb_raster.c) where that ink became
three RGB565 channels. Two consequences worth knowing:

- **White is an exact identity**, not an approximation of the original. Verified by
  sweeping 1,000,001 ink values against the expression it replaced — zero
  mismatches. It is done in float for this reason: the tempting `(lum * ch) >> 8`
  is a level low at small ink. White is no longer in the palette, but it is still
  what an out-of-range index shows.
- **The shell still reads as a surface.** The ink spans a real brightness ramp
  (87..249 across the parity dump's 6,384 dots), and a multiplicative tint keeps
  that as luminance instead of flattening the shell to one flat colour.

Names and values are the brand palette's, lowercased: Fuchsia is `#E042FF`.
(Ed's earlier "Color palette" sheet had `fuschia` at `#E04CFF`; the brand
palette is the source of truth.) A colour saved under a name the palette no
longer has (any of the old thirteen, or `fuschia`) boots as Fuchsia.

## Changing the voice by asking

The agent can change its own voice, and with it the language it speaks. Say
"switch to a British woman" or "talk to me in Spanish" and it picks a voice,
applies it, and remembers it across reboots. "Go back to your default voice"
undoes it.

It is built on four pieces:

- **Client-side function calls.** `agent.think.functions` in the `Settings`
  message declares `set_voice`, `list_voices` and `reset_voice`. Client-side is
  signalled by the *absence* of an `endpoint` field — with one, Deepgram would
  call a web service instead of asking the device.
- **In-place updates.** `UpdateSpeak`, plus `UpdateListen` when the language
  changes and an `UpdatePrompt` line, change the session without reopening it
  (`CONFIG_DEEPGRAM_VOICE_UPDATE_IN_PLACE`, on by default). A session reload is
  the fallback.
- **History replay**, for when a reload does happen. `agent.context.messages`
  carries the recent turns into the new session as
  `{"type":"History","role":...,"content":...}` entries, so the conversation
  continues instead of starting over. Those turns come off flash as readily as
  out of RAM — see [persistence.md](persistence.md).
- **NVS.** Namespace `dgagent`, key `tts_voice`, holding the full model id.

### The voice implies the language

Every voice belongs to one language, and choosing it chooses everything else
that depends on language:

| | English voice | Any other voice |
|---|---|---|
| Listen | `flux-general-en` | `flux-general-multi`, `language_hints: [lang, "en"]`; Japanese and Bulgarian: Nova-3, `language: "ja"` / `"bg"` |
| Prompt | unchanged | "Speak Spanish. The person may also use English; …" (`{{language}}`) |
| Greeting | `CONFIG_DEEPGRAM_AGENT_GREETING` | the language's own, from `voices.c` |

English stays in the hints so "switch back to English" is still heard. Alarm
announcements stay English in every language: they are firmware strings, not
the model's.

**Japanese is the exception: it listens with Nova-3.** `flux-general-multi`
kept the meaning of 13 of 30 Japanese test commands and Nova-3 22 of 30. The
price is that barge-in signals about a second later (+1.5 s against +0.65 s),
and that `language: "ja"` hears native English poorly, so the Japanese prompt
line tells the model English may arrive garbled.

**Bulgarian listens with Nova-3 because nothing else hears it.**
`flux-general-multi` fails the session on a `bg` hint and hears Bulgarian as
English nonsense without one. English through Nova-3 `bg` comes out half
Cyrillic ("Спик English"), so Bulgarian gets the same garbled-English line as
Japanese. "English, please" or "Говори на английски" are the reliable ways
back. Bulgarian is Early Access in Flux TTS, with two voices, both women.

Nova-3 as the agent's listen model also **fails to start about one session in
five** (`FAILED_TO_START_LISTENING`, English as well as Japanese; Flux 0 of 12).
When it does, the device reconnects once with `flux-general-multi` for that
session rather than freezing in "failed", and tries Nova-3 again next session.
In Bulgarian that session is hinted at English alone, so it hears English
only.

**"Speak English"** is the
phrase that works: from three English voices it arrived as スピークイングリッシュ
or `SpeakEnglish` and the model switched voice every time, where "English,
please" was not heard at all. Saying it in Japanese always works. The numbers are in
[notes/languages-probe.md](notes/languages-probe.md).

The languages are English, Spanish, French, German, Hindi, Russian, Portuguese,
Japanese, Italian and Dutch, the ten `flux-general-multi` can hear, and
Bulgarian, which only Nova-3 hears. Flux TTS speaks 42, but a voice in a
language the device cannot understand is a voice nobody can talk back to. Deepgram's Voice Agent page says the agent API takes
English Flux voices only; on this account it took all nine others, measured
with `host/agent_probe.py matrix`. See
[notes/languages-probe.md](notes/languages-probe.md).

### `UpdateSpeak` works now (it did not on 2026-08-21)

When this feature was first built, `UpdateSpeak` returned `SpeakUpdated` and
changed nothing ([notes/updatespeak.md](notes/updatespeak.md)), so the device
applied a voice by reopening the session. Re-measured on 2026-10-28 with
`host/agent_probe.py update-speak`, it works, and so do `UpdateListen` and
`UpdatePrompt`. A voice change is now:

1. The function call arrives; the voice is validated and written to NVS.
2. `UpdateSpeak` goes out, then `UpdateListen` if the language changed, then
   `UpdatePrompt`.
3. Only then the `FunctionCallResponse`. It prompts the agent's next turn, and
   the next turn is when Flux starts using the new voice, so the confirmation
   is the first thing said in the new voice, and in the new language.

**`UpdatePrompt` appends, it does not replace.** The line it sends says "From now
on you are speaking in the voice called carmen", because the `{{voice}}` sentence
in the original prompt is now stale and the newest line has to win. The next
fresh `Settings` builds a clean prompt from the files.

An `Error` or `Warning` within 5 s of sending an update, while its
acknowledgement is still owed, is taken as the update being refused and
answered with a reload; outside that window an `Error` is an `Error`. An
acknowledgement that never comes reloads too, on its own timer. Only
`ListenUpdated` and `PromptUpdated` are waited for: `SpeakUpdated` can be held
back while the server finishes queued audio. A failed send or allocation
reloads instead of half-applying, and after four in-place changes a session
reloads anyway, because each `UpdatePrompt` appends to the server-side prompt.
The voice is already saved, so the fresh `Settings` applies it either way.

### The reload path

With `CONFIG_DEEPGRAM_VOICE_UPDATE_IN_PLACE` off, or as the fallback above:

1. `FunctionCallResponse` tells the agent to say it is changing voice. That
   sentence is spoken in the **old** voice, which is the audible cue.
2. On `AgentAudioDone`, or after an 8 s backstop, `on_reload_required` fires and
   `session_ctl` tears the session down and starts a new one, roughly a
   **1 second gap**.
3. The new `Settings` carries the new voice plus the replayed history, and
   **omits the greeting** so it resumes rather than re-introducing itself.

Turn the option off if a voice change is ever acknowledged but not heard: that
is what the 2026-08-21 server did.

### The history buffer

A **3 kB packed arena** in PSRAM holding whole turns — 25-40 of them in practice,
truncated only past 512 characters each ([dg_agent.c](../main/dg_agent.c)). It is
persisted to flash, so it survives a reboot as well as a reconnect; see
[persistence.md](persistence.md) for the store, the write cadence and the
arithmetic behind the sizes.

What gets replayed into `Settings` is far smaller than what is held: at most
`HISTORY_REPLAY_MAX_TURNS` (6) and `HISTORY_REPLAY_BYTES` (1280), chosen
newest-first so the oldest context is what gets dropped. Those caps are a hard
limit set by measurement — a larger replay pushed `Settings` to 20 kB and made
the session flap, because the message is built and sent at the moment internal
heap is most fragmented. See [persistence.md](persistence.md).

**Nothing clears it implicitly.** A tap-to-stop, a long-press restart, a reload,
the idle timeout and an automatic reconnect all keep the conversation — which is
what fixes the most visible symptom of a dropped socket, where the agent used to
re-greet and forget everything. Forgetting is a deliberate, confirmed act: the
`new_conversation` function below, or hold-again on a stopped device.

### Threading

Sending from inside `handle_json()` — the WebSocket task, mid event dispatch —
is safe: `client->lock` is recursive and that task already owns it, which is why
`send_settings()` has always sent from `WEBSOCKET_EVENT_CONNECTED`. What must
never happen there is a stop/close/destroy; the client refuses those by comparing
task handles. That is why the reload goes out as an `on_reload_required`
callback that only posts to `session_ctl`.

`session_ctl_request_reload()` deliberately **skips the debounce** the touch
gestures use: it comes from the agent acting on the user's words, not from a
finger that might have brushed the bezel, so dropping it would strand the device
on the old setting.

### `CONFIG_DEEPGRAM_LOG_WIRE_JSON`

Off by default. Turn it on to print every JSON body sent to Deepgram — the
fastest way to confirm that what you think you are sending is what actually goes
on the wire. It was how the 2026-08-21 `UpdateSpeak` behaviour got pinned down. The
API key is not in the body; it rides in an HTTP header.

### The catalog

All 151 voices live in [main/voices.c](../main/voices.c): 36 English and 115
across the other ten languages. The model sees three layers:

- **`set_voice`'s description** names the 13 featured English voices, names
  only, and one featured voice per other language ("Spanish (carmen)"), so "a
  French voice" needs no round trip.
- **`list_voices(language)`** returns every voice in one language with what it
  sounds like, in the function response, so neither the catalog nor the
  descriptions ride in `Settings`. A request like "a British woman" or an Irish
  accent goes through it.
- **No enum on `voice`.** A hundred-odd names in every `Settings` would cost more
  than the whole feature, and `voices_find()` refuses anything not in the table
  anyway.

**`Settings` got smaller, not bigger.** Its ceiling is internal heap at send
time, not the wire (see [persistence.md](persistence.md)), and `main` was already
past the 20 kB at which sessions once flapped. A first cut that kept the English
descriptions in `set_voice` added 227 B, so they moved to `list_voices`.
Measured on the board with the same 466 B of history: `main` 20,685 B, this
branch 20,445 B with an English voice (−240) and 20,584 B with a Japanese one
(−101).

`host/voices.sh` checks the table: unique short names across languages, every
model id `flux-<name>-<lang>`, every language one the device can hear (with
`multi` matching the ten `flux-general-multi` takes), one featured
voice per language, and text that fits its buffers. `host/agent_probe.py catalog`
synthesises every model id, so a typo fails on a laptop rather than on a device.

`CONFIG_DEEPGRAM_FLUX_VOICE` is the **factory default**, used on first boot and
after an NVS erase. `ESP_ERR_NVS_NOT_FOUND` on read is the ordinary first-boot
case, not an error.

---

[Back to the README](../README.md)
