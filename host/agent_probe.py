#!/usr/bin/env python3
"""Ask the Agent API what it will actually do, without a board.

    agent_probe.py update-speak OUTDIR [lang]
    agent_probe.py matrix OUTDIR [lang ...]
    agent_probe.py catalog OUTDIR
    agent_probe.py bargein OUTDIR [lang ...]

update-speak  Re-runs the docs/notes/updatespeak.md repro: one session in
              flux-haley-en, UpdateSpeak to flux-cliff-en, two more turns. Each
              turn is written to OUTDIR as a WAV with its median F0 printed as a
              hint -- listening to the WAVs is the evidence, F0 is not. With a
              language, it also runs case B: UpdateSpeak + UpdateListen +
              UpdatePrompt to that language mid-session.

matrix        For each language, synthesises a short question with /v2/speak,
              then plays it into three sessions and records what comes back:
                both    flux-general-multi [lang,en] + flux-<voice>-<lang>
                listen  flux-general-multi [lang,en] + flux-kit-en
                speak   flux-general-en              + flux-<voice>-<lang>
              plus an English control. Writes OUTDIR/matrix.tsv.

catalog       Synthesises one word with every model id in main/voices.c, so a
              mistyped id fails here rather than on someone's device. Checks
              /v2/speak, not the Agent API -- matrix is what says the Agent API
              takes a language at all. Writes OUTDIR/catalog.tsv.

bargein       For each language, has the agent read a long passage and plays the
              question over it two seconds in, as a user talking over the agent
              would. Records whether UserStartedSpeaking arrives and how long
              after the question began -- the event the device's barge-in path
              depends on.

The audio format matches the device (linear16, 16 kHz, container none), so an
answer here is an answer for the firmware. The key comes from DEEPGRAM_API_KEY
and is never printed.
"""
import array
import asyncio
import json
import math
import os
import re
import sys
import time
import urllib.error
import urllib.request
import wave

import websockets

AGENT_URL = "wss://agent.deepgram.com/v1/agent/converse"
SPEAK_URL = "https://api.deepgram.com/v2/speak"
RATE = 16000
CHUNK = RATE * 2 * 80 // 1000          # 80 ms, as on the device
SILENCE = bytes(CHUNK)
THINK = {"provider": {"type": "open_ai", "model": "gpt-5.4-mini"}}

# Languages whose listen side is Nova-3 rather than flux-general-multi, as in
# main/voices.c: flux-general-multi missed about half of a Japanese test set
# that Nova-3 read almost perfectly (docs/notes/languages-probe.md).
NOVA = {"ja"}

# One voice per language, and a question in it for the listen side.
LANGS = {
    "es": ("flux-carmen-es",  "Spanish",    "¿Cuál es la capital de Francia?"),
    "fr": ("flux-camille-fr", "French",     "Quelle est la capitale de l'Espagne ?"),
    "de": ("flux-sonja-de",   "German",     "Was ist die Hauptstadt von Italien?"),
    "hi": ("flux-anjali-hi",  "Hindi",      "भारत की राजधानी क्या है?"),
    "ru": ("flux-olga-ru",    "Russian",    "Какая столица Германии?"),
    "pt": ("flux-bianca-pt",  "Portuguese", "Qual é a capital da Espanha?"),
    "ja": ("flux-ayumi-ja",   "Japanese",   "フランスの首都はどこですか？"),
    "it": ("flux-sofia-it",   "Italian",    "Qual è la capitale della Francia?"),
    "nl": ("flux-anouk-nl",   "Dutch",      "Wat is de hoofdstad van Duitsland?"),
}

PROMPT = "You are a helpful voice assistant. Answer in one short sentence."


def key():
    return os.environ["DEEPGRAM_API_KEY"]


def listen_block(lang):
    if lang in NOVA:
        return {"provider": {"type": "deepgram", "version": "v1", "model": "nova-3",
                             "language": lang}}
    p = {"type": "deepgram", "version": "v2"}
    if lang == "en":
        p["model"] = "flux-general-en"
    else:
        p["model"] = "flux-general-multi"
        p["language_hints"] = [lang, "en"]
    return {"provider": p}


def speak_block(model):
    return {"provider": {"type": "deepgram", "version": "v2", "model": model}}


def language_line(lang):
    """The firmware's {{language}} line -- keep in step with voices.c."""
    if lang == "en":
        return ""
    line = f" Reply in {LANGS[lang][1]} unless asked otherwise."
    if lang in NOVA:
        line += (" English may arrive garbled, like peakenglish; a request for "
                 "English means switch voice.")
    return line


def settings(listen_lang, speak_model, prompt=PROMPT, greeting=None):
    agent = {
        "listen": listen_block(listen_lang),
        "think": dict(THINK, prompt=prompt),
        "speak": speak_block(speak_model),
    }
    if greeting:
        agent["greeting"] = greeting
    return {
        "type": "Settings",
        "audio": {
            "input": {"encoding": "linear16", "sample_rate": RATE},
            "output": {"encoding": "linear16", "sample_rate": RATE, "container": "none"},
        },
        "agent": agent,
    }


def synth(model, text):
    """/v2/speak batch, raw linear16 at the device rate."""
    url = f"{SPEAK_URL}?model={model}&encoding=linear16&sample_rate={RATE}&container=none"
    req = urllib.request.Request(url, data=json.dumps({"text": text}).encode(),
                                 headers={"Authorization": f"Token {key()}",
                                          "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def write_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)


def median_f0(pcm):
    """Autocorrelation pitch over voiced frames, 70-400 Hz. A hint only."""
    s = array.array("h", pcm)
    n, hop = 640, 2560                   # 40 ms frames, every 160 ms
    lo, hi = RATE // 400, RATE // 70
    peak = max((abs(x) for x in s), default=0)
    f0s = []
    for start in range(0, len(s) - n - hi, hop):
        fr = s[start:start + n + hi]
        energy = sum(x * x for x in fr[:n])
        if energy < (0.05 * peak) ** 2 * n:
            continue
        best, best_lag = 0.0, 0
        for lag in range(lo, hi):
            c = 0
            for i in range(0, n, 2):
                c += fr[i] * fr[i + lag]
            if c > best:
                best, best_lag = c, lag
        if best_lag and best > 0.3 * energy / 2:
            f0s.append(RATE / best_lag)
    if not f0s:
        return None
    f0s.sort()
    return f0s[len(f0s) // 2]


class Session:
    """One Agent API socket, with a pump that keeps the uplink fed with
    silence (or with `play` audio) and a log of every server message."""

    def __init__(self, ws):
        self.ws = ws
        self.events = []
        self.audio = bytearray()
        self.queue = asyncio.Queue()
        self.uplink = bytearray()

    async def recv_loop(self):
        try:
            async for raw in self.ws:
                if isinstance(raw, bytes):
                    self.audio += raw
                    continue
                m = json.loads(raw)
                self.events.append(m)
                await self.queue.put(m)
        except websockets.ConnectionClosed as e:
            await self.queue.put({"type": "_closed", "code": e.code, "reason": e.reason})

    async def pump(self):
        try:
            while True:
                if self.uplink:
                    chunk = bytes(self.uplink[:CHUNK])
                    del self.uplink[:CHUNK]
                    chunk = chunk.ljust(CHUNK, b"\0")
                else:
                    chunk = SILENCE
                await self.ws.send(chunk)
                await asyncio.sleep(0.08)
        except websockets.ConnectionClosed:
            pass

    async def send(self, msg):
        await self.ws.send(json.dumps(msg))

    async def wait(self, types, timeout):
        """Next message whose type is in `types`, or an Error/close, or None."""
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return None
            try:
                m = await asyncio.wait_for(self.queue.get(), left)
            except asyncio.TimeoutError:
                return None
            if m.get("type") in types or m.get("type") in ("Error", "_closed"):
                return m

    async def turn(self, timeout=20):
        """Audio from now until AgentAudioDone."""
        mark = len(self.audio)
        m = await self.wait({"AgentAudioDone"}, timeout)
        return bytes(self.audio[mark:]), m


async def open_session(cfg):
    ws = await websockets.connect(AGENT_URL, additional_headers={"Authorization": f"Token {key()}"},
                                  max_size=None)
    s = Session(ws)
    tasks = [asyncio.create_task(s.recv_loop())]
    m = await s.wait({"Welcome"}, 10)
    await s.send(cfg)
    applied = await s.wait({"SettingsApplied"}, 10)
    tasks.append(asyncio.create_task(s.pump()))
    return s, ws, tasks, applied


async def close_session(ws, tasks):
    for t in tasks:
        t.cancel()
    await ws.close()


def describe(m):
    if m is None:
        return "timeout"
    t = m.get("type")
    if t == "Error":
        return f"Error {m.get('code', '')}: {m.get('description') or m.get('message')}"
    if t == "_closed":
        return f"closed {m.get('code')} {m.get('reason')}"
    return t


# ---------------------------------------------------------------- update-speak

SAY = "Say exactly: one, two, three, four, five."


async def inject_turn(s, label, outdir):
    await s.send({"type": "InjectAgentMessage", "message": "One, two, three, four, five."})
    pcm, end = await s.turn()
    path = os.path.join(outdir, f"{label}.wav")
    write_wav(path, pcm)
    f0 = median_f0(pcm)
    f0s = f"{f0:5.0f} Hz" if f0 else "   -    "
    print(f"  {label:<10} {len(pcm):7d} B  F0 {f0s}  end={describe(end)}  {path}", flush=True)
    return f0


async def case_a(outdir):
    print("case A: flux-haley-en -> UpdateSpeak flux-cliff-en", flush=True)
    s, ws, tasks, applied = await open_session(settings("en", "flux-haley-en"))
    print(f"  Settings: {describe(applied)}")
    try:
        await inject_turn(s, "A-before", outdir)
        await s.send({"type": "UpdateSpeak", "speak": speak_block("flux-cliff-en")})
        ack = await s.wait({"SpeakUpdated"}, 10)
        print(f"  UpdateSpeak: {describe(ack)}", flush=True)
        await inject_turn(s, "A-after1", outdir)
        await inject_turn(s, "A-after2", outdir)
    finally:
        await close_session(ws, tasks)


async def case_b(outdir, lang):
    voice, name, question = LANGS[lang]
    print(f"case B: flux-haley-en -> {voice} + flux-general-multi [{lang},en] + UpdatePrompt",
          flush=True)
    q = synth(voice, question)
    s, ws, tasks, applied = await open_session(settings("en", "flux-haley-en"))
    print(f"  Settings: {describe(applied)}")
    try:
        await inject_turn(s, "B-before", outdir)
        await s.send({"type": "UpdateSpeak", "speak": speak_block(voice)})
        print(f"  UpdateSpeak: {describe(await s.wait({'SpeakUpdated'}, 10))}", flush=True)
        await s.send({"type": "UpdateListen", "listen": listen_block(lang)})
        print(f"  UpdateListen: {describe(await s.wait({'ListenUpdated'}, 10))}", flush=True)
        await s.send({"type": "UpdatePrompt", "prompt": language_line(lang).strip()})
        print(f"  UpdatePrompt: {describe(await s.wait({'PromptUpdated'}, 10))}", flush=True)
        mark = len(s.events)
        s.uplink += q
        pcm, end = await s.turn(timeout=30)
        write_wav(os.path.join(outdir, "B-after.wav"), pcm)
        for m in s.events[mark:]:
            if m.get("type") == "ConversationText":
                print(f"  {m.get('role'):<9} {m.get('content')!r}", flush=True)
        f0 = median_f0(pcm)
        print(f"  B-after    {len(pcm):7d} B  F0 {f0 or 0:5.0f} Hz  end={describe(end)}",
              flush=True)
    finally:
        await close_session(ws, tasks)


async def update_speak(outdir, lang=None):
    await case_a(outdir)
    if lang:
        await case_b(outdir, lang)


# ---------------------------------------------------------------------- matrix

async def probe(listen_lang, speak_model, question_pcm):
    """One session: play the question, collect what comes back."""
    row = {"settings": "", "user": "", "agent": "", "audio": 0, "errors": ""}
    try:
        s, ws, tasks, applied = await open_session(settings(listen_lang, speak_model))
    except Exception as e:  # handshake refusals land here
        row["settings"] = f"connect failed: {e}"
        return row
    row["settings"] = describe(applied)
    try:
        if applied is None or applied.get("type") != "SettingsApplied":
            return row
        # Half a second of silence first: playing the instant the session is
        # live clipped the first word in every language, the control included.
        s.uplink += bytes(RATE) + question_pcm
        mark = len(s.audio)
        await s.turn(timeout=25)
        row["audio"] = len(s.audio) - mark
    finally:
        await close_session(ws, tasks)
    for m in s.events:
        t = m.get("type")
        if t == "ConversationText":
            row[m.get("role") == "user" and "user" or "agent"] += m.get("content", "")
        elif t in ("Error", "Warning"):
            row["errors"] += describe(m) + "; "
    return row


async def matrix(outdir, langs):
    rows = []
    q_en = synth("flux-kit-en", "What is the capital of France?")
    r = await probe("en", "flux-kit-en", q_en)
    rows.append(("en", "control", "flux-general-en", "flux-kit-en", r))
    print(f"en control: {r}", flush=True)
    for lang in langs:
        voice = LANGS[lang][0]
        try:
            q = synth(voice, LANGS[lang][2])
        except urllib.error.HTTPError as e:
            print(f"{lang}: /v2/speak refused {voice}: {e.code}", flush=True)
            rows.append((lang, "synth", "-", voice, {"settings": f"/v2/speak {e.code}",
                                                     "user": "", "agent": "", "audio": 0,
                                                     "errors": ""}))
            continue
        for kind, ll, sm in (("both", lang, voice),
                             ("listen", lang, "flux-kit-en"),
                             ("speak", "en", voice)):
            r = await probe(ll, sm, q)
            lp = listen_block(ll)["provider"]
            rows.append((lang, kind, lp["model"] + ("/" + lp["language"] if "language" in lp else ""),
                         sm, r))
            print(f"{lang} {kind:<6}: {r}", flush=True)

    path = os.path.join(outdir, "matrix.tsv")
    with open(path, "w") as f:
        f.write("lang\tcase\tlisten\tspeak\tsettings\taudio_bytes\tuser\tagent\terrors\n")
        for lang, kind, lm, sm, r in rows:
            f.write("\t".join([lang, kind, lm, sm, r["settings"], str(r["audio"]),
                               r["user"], r["agent"], r["errors"]]) + "\n")
    print(f"wrote {path}")


# --------------------------------------------------------------------- bargein

PASSAGE = ("Here is a long story. Once upon a time, in a small village by the sea, "
           "there lived an old fisherman who went out every morning before dawn. "
           "He knew every rock and every current, and the other fishermen asked "
           "him for advice whenever the weather turned. One winter, a storm came "
           "that nobody expected, and the whole village waited on the shore.")


async def bargein_one(lang, voice, question):
    s, ws, tasks, applied = await open_session(settings(lang, voice))
    try:
        if applied is None or applied.get("type") != "SettingsApplied":
            return f"Settings: {describe(applied)}"
        await s.send({"type": "InjectAgentMessage", "message": PASSAGE})
        # Wait for the agent to actually be speaking before talking over it.
        end = time.monotonic() + 15
        while len(s.audio) < RATE * 2 * 2 and time.monotonic() < end:
            await asyncio.sleep(0.05)
        mark = len(s.events)
        t0 = time.monotonic()
        s.uplink += question
        m = await s.wait({"UserStartedSpeaking"}, 8)
        dt = time.monotonic() - t0
        heard = ""
        await asyncio.sleep(6)
        for e in s.events[mark:]:
            if e.get("type") == "ConversationText" and e.get("role") == "user":
                heard = e.get("content", "")
        if m is not None and m.get("type") == "UserStartedSpeaking":
            return f"UserStartedSpeaking +{dt:.2f}s  heard={heard!r}"
        return f"no UserStartedSpeaking ({describe(m)})  heard={heard!r}"
    finally:
        await close_session(ws, tasks)


async def bargein(langs):
    q_en = synth("flux-kit-en", "What is the capital of France?")
    print(f"en  flux-general-en: {await bargein_one('en', 'flux-hannah-en', q_en)}", flush=True)
    for lang in langs:
        voice, _, question = LANGS[lang]
        q = synth(voice, question)
        model = listen_block(lang)["provider"]["model"]
        print(f"{lang}  {model}: {await bargein_one(lang, voice, q)}", flush=True)


# --------------------------------------------------------------------- catalog

def catalog(outdir):
    src = os.path.join(os.path.dirname(__file__), "..", "main", "voices.c")
    models = re.findall(r'"(flux-[a-z]+-[a-z]{2})"', open(src).read())
    bad = []
    with open(os.path.join(outdir, "catalog.tsv"), "w") as f:
        f.write("model\tresult\n")
        for m in models:
            try:
                n = len(synth(m, "Hello."))
                result = f"ok {n} B" if n > 0 else "empty"
            except urllib.error.HTTPError as e:
                result = f"HTTP {e.code}"
            if not result.startswith("ok"):
                bad.append((m, result))
            f.write(f"{m}\t{result}\n")
    print(f"{len(models)} models, {len(bad)} refused")
    for m, r in bad:
        print(f"  {m}: {r}")
    return 1 if bad else 0


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    mode, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    if mode == "update-speak":
        asyncio.run(update_speak(outdir, sys.argv[3] if len(sys.argv) > 3 else None))
    elif mode == "matrix":
        asyncio.run(matrix(outdir, sys.argv[3:] or list(LANGS)))
    elif mode == "bargein":
        asyncio.run(bargein(sys.argv[3:] or list(LANGS)))
    elif mode == "catalog":
        sys.exit(catalog(outdir))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
