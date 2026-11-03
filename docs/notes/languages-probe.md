# Which Flux languages the Agent API takes

> Measured 2026-10-28 with `host/agent_probe.py matrix`, against
> `/v1/agent/converse` on this project's account. Re-run before adding a
> language, or if a language stops working.

## Why it was measured

Deepgram's Voice Agent TTS page says the Agent API "currently supports the
English Flux TTS voices", with the other languages "available on /v2/speak only
for now". The device only talks to the Agent API, so taken at face value that
would have ruled out every non-English voice. It was cheaper to ask the server.

Only the nine non-English languages `flux-general-multi` can hear were tried:
es, fr, de, hi, ru, pt, ja, it, nl. Flux TTS speaks 42 languages, but the
device's design is that the voice implies the language, so a voice it cannot
hear an answer in is no use.

## Method

For each language, one question was synthesised with `/v2/speak` in that
language's voice, then played in real time (80 ms chunks, linear16, 16 kHz)
into three sessions:

| case | listen | speak |
|---|---|---|
| both | `flux-general-multi`, `language_hints [lang,"en"]` | `flux-<voice>-<lang>` |
| listen | the same | `flux-kit-en` |
| speak | `flux-general-en` | `flux-<voice>-<lang>` |

`both` is what the device sends. `listen` and `speak` isolate a failure if
`both` had one. Recorded: `SettingsApplied` or `Error`, the user and agent
`ConversationText`, and the audio bytes that came back.

## Result: all nine pass

Every session got `SettingsApplied` and no `Error` or `Warning`. Every `both`
session heard the question, answered in the language, and returned audio.

| lang | voice | heard (both) | answered (both) | audio |
|---|---|---|---|---|
| en | `flux-kit-en` (control) | Watt is the capital of France. | The capital of France is Paris. | 89,600 B |
| es | `flux-carmen-es` | es la capital de Francia? | La capital de Francia es París. | 69,120 B |
| fr | `flux-camille-fr` | est la capitale de l'Espagne | La capitale de l'Espagne est Madrid. | 69,120 B |
| de | `flux-sonja-de` | Was ist die Hauptstadt von Italien? | Die Hauptstadt von Italien ist Rom. | 69,120 B |
| hi | `flux-anjali-hi` | भारत की राजधानी क्या है? | भारत की राजधानी नई दिल्ली है। | 76,800 B |
| ru | `flux-olga-ru` | Какая столица Германии? | Столица Германии — Берлин. | 71,680 B |
| pt | `flux-bianca-pt` | qual é a capital da Espanha? | A capital da Espanha é Madri. | 66,560 B |
| ja | `flux-ayumi-ja` | フランスの下とはどこですか? | (answered the misheard question) | 212,480 B |
| it | `flux-sofia-it` | qual è la capitale della Francia? | La capitale della Francia è Parigi. | 81,920 B |
| nl | `flux-anouk-nl` | Wat is de hoofdstad van Duitsland? | De hoofdstad van Duitsland is Berlijn. | 87,040 B |

Notes:

- **Japanese STT misheard 首都 (capital) as 下と** in both sessions that used
  `flux-general-multi`, and the model answered the misheard question. That
  prompted the battery below, and Japanese now listens with Nova-3.
- The `speak` column (English STT hearing another language) failed as
  expected: nothing heard, or a phonetic English guess. It confirms that
  changing the voice without changing the listen model would be useless, which
  is the design's premise.
- The first word or two is sometimes clipped from what was heard ("es la
  capital…"). That is the probe starting playback the moment the session is
  live, not a language problem: the English control lost a word the same way.

## The five-sentence battery

One sentence is a thin basis for a language, so every language got the same
five: two capitals, the weather, an alarm at seven tomorrow morning, and
"volume up a little". Each was synthesised in two voices (one woman, one man)
and streamed to `/v2/listen` with `flux-general-multi` and the device's hints,
with Nova-3 reading the same audio as a reference. Synthetic speech, no mic.

| lang | Flux CER | Nova-3 CER | meaning lost (of 10) |
|---|---|---|---|
| en | 0% | 0% | 0 |
| pt, de | 0% | 0–5% | 0 |
| ru | 3% | 1% | 0 ("7" for семь; a politer verb form) |
| it | 2% | 9% | 0 ("Alzampò il volume", intent intact) |
| es | 3% | 1% | 1, borderline ("Doy un poco el volumen") |
| fr | 3% | 1% | 1 (weather question lost) |
| nl | 5% | 3% | 1 (weather question lost) |
| hi | 18% | 16% | 0 -- the CER is English loanwords written in Latin, "France की राजधानी", as Nova-3 does too |
| **ja** | **19%** | **3%** | **5, plus 1 partial** |

## Japanese listens with Nova-3

Japanese was the only language that fell apart, and on exactly the device's own
commands: 首都 (capital) as 人 (person) or 外 (outside), 音量を上げて (volume up) as
本音 (true feelings), alarm sentences losing 明日 (tomorrow) or turning to
nonsense with Latin text in it. The voices are not the cause: Nova-3 reads the
same audio well.

Batch Nova-3 looked near-perfect (11 of 12), but the agent streams, so the
decision was made on streaming. Five sentences, two voices, three runs, scored
on whether the key words survived:

| | meaning kept |
|---|---|
| `flux-general-multi`, hints `[ja, en]` | 13 / 30 |
| Nova-3, `language: ja`, streaming | 22 / 30 |

Better, not solved: streaming Nova-3 still misses 日本の首都 more often than not,
and sometimes 明日. It also drops さ from ください throughout, which costs nothing.

**`ja`, not `multi`.** `multi` heard English perfectly and Japanese worse (8 of
12 batch, with 首都 → 人 back). `ja` hears native English poorly or not at all:
"Switch back to an English voice" in an American voice came back empty. Simple
phrases do get through spelled out -- "Speak English" arrived as `peakenglish`
or `speakenglish` from all three English voices tried, batch -- so the Japanese
prompt line tells the model that a garbled request for English means switch
back. Checked end to end in the Agent API with a `set_voice` tool and that line:
"Speak English" from kit, hannah and cliff arrived as スピークイングリッシュ,
`SpeakEnglish` and スピーキングリッシュ, and the model called `set_voice` all three
times. "English, please" was not heard at all. Saying it in Japanese (英語の声に戻してください) is heard
cleanly. Nova-3 keyterms ("English", "reset your voice") changed little and are
not used.

**Barge-in is slower.** With the agent reading a long passage and the question
played over it two seconds in, `UserStartedSpeaking` arrived:

| listen | UserStartedSpeaking after the user starts |
|---|---|
| `flux-general-en` | +0.63 to +0.82 s (5 runs) |
| `flux-general-multi` (es) | +0.30 s (1 run) |
| Nova-3 `ja` | +1.47 to +1.65 s (4 runs) |

So a Japanese speaker talking over the agent hears it carry on for roughly a
second longer. It does stop.

**In the Agent API**, Nova-3 `ja` with `flux-ayumi-ja` answered "フランスの首都は
どこですか" correctly; `UpdateListen` switched Flux → Nova-3 and Nova-3 → Flux
mid-session, each acknowledged and each followed by a correctly heard question;
and the firmware's own Japanese `Settings` was applied (20,584 B after the
trim that followed, against `main`'s 20,685 B on the same history).

## Nova-3 sometimes will not start, in any language

Testing the in-place switch on the board turned up `FAILED_TO_START_LISTENING`
from Nova-3. It looked at first like timing, then like microphone noise, and was
neither: it is intermittent. Twelve rounds of fresh laptop sessions, 2026-10-28
13:36-13:40:

| listen | failed to start |
|---|---|
| Nova-3 `ja` | 2 of 12 |
| Nova-3 `en` | 3 of 12 |
| `flux-general-multi` `[ja, en]` | 0 of 12 |

Device sessions with request ids: `01a18431-c2fa-7980-acff-da1af779e74c` (an
in-place `UpdateListen` refused) and `01a18431-ebb8-7e42-b8ca-fd2b84074783` (the
fresh session after it, refused again).

Treated as an ordinary `Error`, that froze the device in "failed" until a hold
or BOOT press. **The firmware now falls back:** on `FAILED_TO_START_LISTENING`
while listening with Nova-3, it reconnects once with `flux-general-multi`
`[ja, en]` for that session, and the session after tries Nova-3 again. Verified
on the board by forcing the failure (Nova-3 with `language: "zz"`, which fails
3 of 3): live on Flux 1.6 s after the refusal from a fresh session, 3.5 s from an
in-place switch.

**Always a reconnect, never an in-place repair.** The first version answered a
refused in-place `UpdateListen` with another `UpdateListen`, to Flux, on the
same socket. It was acknowledged, and then the uplink clogged: `send queue full`
from then on, 50-63 frames dropped in two runs of two, one ending in a write
timeout. A fresh session does not have the problem.

**`SpeakUpdated` is not reliable as an acknowledgement.** Sent straight after
`SettingsApplied`, `UpdateSpeak` was not acknowledged within 6 s (2 of 2) where
it was 3 s later -- the server finishes queued audio first. The firmware's
missing-acknowledgement timer therefore waits only on `ListenUpdated` and
`PromptUpdated`.

## Bulgarian, through Nova-3 only

Added 2026-10-29, after "speak Bulgarian" was refused on the board. Deepgram's
[Flux TTS voices page](https://developers.deepgram.com/docs/flux-tts/voices)
lists Bulgarian in Early Access with two voices, `flux-desislava-bg` and
`flux-gergana-bg`, both women. Both synthesise through `/v2/speak`.

`flux-general-multi` cannot be the listen side:

| listen | heard "Коя е столицата на Франция?" as |
|---|---|
| `flux-general-multi`, hints `[bg, en]` | nothing: `INTERNAL_SERVER_ERROR` (1 of 1; with English requests, 7 of 9, the other 2 heard nothing) |
| `flux-general-multi`, hints `[en]` | "Korea's toilet is in Francia." |
| `flux-general-multi`, no hints | "Goeiaar, stolit het in Francia?" (answered in Dutch) |
| Nova-3, `language: bg` | Коя е столицата на Франция? (2 of 2) |

So Bulgarian listens with Nova-3 like Japanese, and `language_t.multi` is false
for it: the Nova-3 fallback hints English alone rather than send a hint that
fails the session. A fallback session in Bulgarian hears English only.

`matrix` with `flux-desislava-bg`: `both` and `listen` heard the question word
for word and answered "Столицата на Франция е Париж." with audio; `speak`
(English STT) heard "Guillaire stole it at the novante." as expected.

**English through Nova-3 `bg`** comes out half Cyrillic. From kit, cliff and
hannah: "Speak English" as `Speedк`, `Спик English.`, `Спик,`; "Switch to
Hannah" as `Суичтахана!`, `Свичте хена.`, `Switched Hera.`; "English, please"
as `English Plays`, `English prease`, `English Please,`. Injected into the
Agent API with the device's prompt, Bulgarian language line and voice tools,
7 of 9 requests landed: "Speak Bulgarian", "Can you talk to me in Bulgarian?"
and "Switch to Gergana" from English; `Спик English.`, both "English please"
garbles and "Говори на английски." back. The misses were `Speedк` and
`Суичтахана!`, which nothing could read as English. The Japanese line about
garbled English is reused unchanged.

**No text normalisation**, per Deepgram's page: numbers and symbols are read as
written. Transcribing the voice's own output with Nova-3, digits read fine
("14:30" as четиринадесет и трийсет, "7 ч." as седем часа) but "°C" did not.
The model already writes these out in words under the existing prompt ("сто
градуса по Целзий", three of three questions), so nothing was added for it.

## Every model id

`host/agent_probe.py catalog` synthesised one word with every model id in
`main/voices.c` through `/v2/speak`: 149 voices, none refused (before Bulgarian's two). That checks the
ids exist; it does not re-check the Agent API, which `matrix` covered one voice
per language.

## On the device

With a temporary build that forced `flux-carmen-es` at boot, the firmware's own
`Settings` (21,198 B before the trim) carried
`"listen":{"provider":{"type":"deepgram","version":"v2","model":"flux-general-multi","language_hints":["es","en"]}}`
and `"speak":{"provider":{"type":"deepgram","version":"v2","model":"flux-carmen-es"}}`,
and got `SettingsApplied`. Speaking to it in Spanish is in the attended
verification, not here.
