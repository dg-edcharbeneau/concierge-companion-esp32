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

## Limits and known gaps

- **Latency.** A concierge answer needs two model hops: the device's think
  model, then the concierge workflow's model. Expect the desk's own latency
  (concierge ADR 0008 measured 2–17 s) plus about 1–3 s.
- **Rewording.** The device model rewords the desk's answers. The prompt
  requires every fact, time and place to be kept; check this live,
  especially for ride confirmations.
- **One question at a time.** A second `ask_concierge` while one is still
  running gets a "still answering" error.
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
