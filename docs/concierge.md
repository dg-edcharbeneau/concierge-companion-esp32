# Hotel Concierge companion

This firmware turns the board into an in-room companion for the Hotel ZGY
concierge (the .NET/Aspire `concierge-agent` app). The device keeps everything
the upstream agent does: its own Deepgram voice agent, its own think model,
and all of its local functions (volume, voice, colour, clock, timers). It
gains one more function, `ask_concierge`, which hands hotel, stay, area,
weather, route and ride requests to the concierge desk and speaks the answer
that comes back.

```
guest ── voice ──▶ device ══ wss ══▶ Deepgram Voice Agent (STT · think LLM · TTS)
                     │   ◀── FunctionCallRequest ask_concierge{request}
                     └── https ──▶ concierge  POST /api/concierge/sessions  (last name + room → credential)
                                              POST /api/concierge/turns     (Bearer, channel "voice")
                     ◀── FunctionCallResponse {content: the desk's answer}
```

The concierge has no device-specific code. To it, this is just another client
of its public guest API. See "Remote API clients" in the concierge's
`docs/configuration.md`.

## Setup

1. **Start the concierge with the API tunnel.** In the concierge repo, set
   `RemoteAccess:PublicTunnel` to `true` in `src/Concierge.AppHost/appsettings.json`
   and start the AppHost. Copy the URL of the `api-tunnel` resource from the
   Aspire dashboard. It's an `https://…devtunnels.ms` address.
2. **Build and flash.** Follow [build.md](build.md). This repo's defaults
   (`sdkconfig.defaults.concierge`) enable the concierge component.
3. **Provision.** Join the device's setup network and fill in the page:
   - Wi-Fi and the Deepgram API key, as upstream.
   - **Concierge address**: the tunnel URL from step 1.
   - **Guest last name** and **Room number**: a stay that is currently
     eligible. The demo stays are Rivera 412, Morgan 812 and Phillip 112.

   Leave a field blank to keep the value already saved.

For bench builds, the same three values can be seeded in `idf.py menuconfig`
under **Concierge Companion**. Values saved through the portal always take
precedence. Never commit them in `sdkconfig.defaults.concierge`.

## How a concierge turn works

1. The device's model decides the request is for the desk. It says a short
   holding line and calls `ask_concierge` with the guest's words.
2. `dg_agent.c` passes the call to `concierge_tool_handle_async()` and goes
   straight back to the WebSocket. A worker task makes the HTTPS call. It
   lives only for the call, on a 6 kB internal-RAM stack.
3. On the first question after boot, or after any settings change, the worker
   verifies the guest (`POST /sessions`) and caches the credential in NVS.
4. `POST /turns` sends `turnId: "dev:<128-bit random>"` and `channel: "voice"`.
   On a `401` (the stay ended, was reset or was ended), the worker verifies
   again and retries once with the same `turnId`. The stay workflow joins
   exact retries, so a ride isn't booked twice.
5. The answer goes back as the `FunctionCallResponse`. If the agent connection
   was replaced in the meantime, the answer is dropped, because an old
   function ID sent on a new connection would come back as a server `Error`.

Rides are always proposed and confirmed by the desk. The prompt tells the
device model to pass the guest's yes, no or change back through
`ask_concierge`, and never to confirm anything itself.

The device and the browser UI can be signed in to the same stay at the same
time. They share one stay workflow, so the browser shows a ride booked from
the device.

## TLS to dev tunnels

`*.devtunnels.ms` chains to **Microsoft TLS RSA Root G2**, which isn't in IDF 5.5.5's CA bundle. The
tunnel does send a DigiCert cross-signed copy of that root. But it sends it before the intermediate it
signs, and mbedTLS only looks for an issuer later in the chain. The handshake then fails with
`esp-x509-crt-bundle: Failed to verify certificate` / `-0x3000`. Curl and browsers rebuild the chain in
any order, which is why the URL works from a laptop.

The fix is to add the root to the bundle through `CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE`. The
certificate came from Microsoft's PKI repository (SHA-256 `6A:17:05:83…E9:E5`, valid until 2040), and its
key matches the cross-signed copy the tunnel presents. If the tunnel ever changes CA, the same error
comes back. Compare `openssl s_client -showcerts` against the bundle.

## Errors the device speaks

When `ask_concierge` fails, the function result starts with `DESK ERROR:`. The
prompt has the model explain the problem briefly instead of reading it out word
for word.

| Cause | What the guest hears (roughly) | Fix |
| --- | --- | --- |
| Address, last name or room not set | Not linked to a hotel yet | Run the setup portal. |
| DNS, TLS, connection or timeout failure | Couldn't reach the concierge | Check the Wi-Fi, that the AppHost is running and that the `api-tunnel` URL hasn't changed. |
| `404 guest_not_found` | No current stay matches | Re-enter the last name and room; the stay must still be eligible. |
| `401` after re-verifying | The desk ended the session | Run setup again for a new stay. |
| `400`/`409` | Couldn't take that request | Rephrase it. Messages are capped at 2,000 bytes. |
| `5xx` | Temporarily unavailable | Check the concierge's WorkflowServer and Temporal in the Aspire dashboard. |

Serial logs use the `concierge` tag. The credential is never logged.

## Latency (bench, 2026-11-04)

Measured from the end of the guest's speech to the device speaking the answer: **3.5–6 s**.

| Stage | System | Time |
|---|---|---|
| Decide to call `ask_concierge` | Device think LLM (`gpt-5.4-mini`) | 0.5–0.7 s |
| Network | Dev tunnel + TLS (connection reused) | 0.45–1.45 s; about 1 s more on a new handshake |
| Retrieval embedding | Foundry `text-embedding-3-small` | 0.13–0.26 s |
| Answer model | Foundry `gpt-5-mini` | 1.4–2.7 s; about 1 s on `gpt-5.4-mini` |
| Tool (weather, places, ride) | Concierge activity | 0.15–0.6 s |
| Speak the answer | Device think LLM, second step | 0.6–1.1 s |

The holding line plays about 0.9 s after the guest stops speaking, which covers most of that wait.

## Echo and self-interruption

Two things made the device hear its own voice as a barge-in, and both are fixed:

- **A misadapted echo-cancellation block:** one where the cancelled output is louder than the raw
  microphone, which can't happen with real speech. Such a block can no longer open the uplink gate. The
  log shows `EVT aec block louder than mic` when the guard fires.
- **Amplifier wake-ups:** upstream switches the amplifier off after 5 s of silence, and a desk wait is
  longer than that. Each wake-up changes the echo path, and on the bench half of them were followed by
  misadapted blocks. `AUDIO_PA_IDLE_OFF_MS=60000` keeps the amplifier on through a conversation.

Both changes are proposed upstream in esp32-deepgram-voice-agent#14.

## Limits and known gaps

- **Holding line in the model's context.** The firmware queues "One moment, let me check with the
  desk." as an agent message, so it ends up in the device model's conversation context. On the bench,
  the model once said that line by itself and never called `ask_concierge`.

- **Latency.** A concierge answer needs two model hops: the device's think
  model, then the concierge workflow's model. Expect the desk's own latency
  (concierge ADR 0008 measured 2–17 s) plus about 1–3 s.
- **Rewording.** The device model rewords the desk's answers. The prompt
  requires every fact, time and place to be kept; check this live,
  especially for ride confirmations.
- **Questions run one at a time.** One `ask_concierge` call runs and one waits behind it. A third
  gets a "still answering" error. A call cancelled by barge-in (`FunctionCallCancelled`) has its
  answer dropped.
- **Turn timeout.** `CONFIG_CONCIERGE_TURN_TIMEOUT_MS` defaults to 25 s.
  Deepgram's own timeout for a client-side function result hasn't been
  measured yet (see the smoke test).
- **Not implemented:** ride status or transcript on the display (the
  concierge has no feed endpoint for that yet), calling `POST /end` when a
  session stops, and Deepgram temporary tokens.

## Smoke test

Run this after every upstream merge.

1. Say "turn the volume up". A local function should still work.
2. Ask "what time does the pool open?" You should hear a holding line, then
   the desk's answer.
3. Ask "what's the weather tomorrow?"
4. Say "book me a ride to the airport at 3pm", then "yes". Check that the ride
   appears in the browser UI for the same stay.
5. Interrupt a desk answer by talking over it (barge-in).
6. Stop the AppHost and ask a desk question. You should hear the
   "couldn't reach" error.
7. Save a wrong room number in the portal and ask a desk question. You should
   hear the "no current stay" error.
