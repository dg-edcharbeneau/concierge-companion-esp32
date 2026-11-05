# Working with upstream

This repo is a downstream copy of
[esp32-deepgram-voice-agent](https://github.com/dg-edcharbeneau/esp32-deepgram-voice-agent).
It shares that repo's history but is not a GitHub fork, because GitHub won't
fork a repo into the account that already owns it. Changes only go one way:
upstream into this repo. Nothing from here gets pushed upstream.

## Remotes

| Remote | URL | Push |
| --- | --- | --- |
| `origin` | `https://github.com/dg-edcharbeneau/concierge-companion-esp32.git` | yes |
| `upstream` | `https://github.com/dg-edcharbeneau/esp32-deepgram-voice-agent.git` | disabled (`DISABLED` push URL) |

To set up a fresh clone the same way:

```sh
git remote add upstream https://github.com/dg-edcharbeneau/esp32-deepgram-voice-agent.git
git remote set-url --push upstream DISABLED
```

## Pulling upstream changes

```sh
git fetch upstream
git switch -c merge-upstream-$(date +%Y%m%d)
git merge upstream/main          # merge, not rebase: keeps the shared history intact
idf.py build                     # or the CI container: espressif/idf:v5.5.5
```

Then flash and run the smoke test in [docs/concierge.md](docs/concierge.md#smoke-test)
before merging into `main`.

Upstream tags (`v*`) are not pushed here. Companion releases use
`companion-v*`, which is what `.github/workflows/release.yml` triggers on.

## Where the fork touches upstream files

Everything companion-specific lives in files that upstream doesn't have, so
those files can't conflict:

- `components/concierge/`: the component (Kconfig, NVS settings, HTTPS client, `ask_concierge`, the PSRAM worker, hold lines and recovery in `concierge_hold.c`, prompt blocks)
- `host/concierge_hold.sh`, `host/concierge_hold_test.c`: host tests for `concierge_hold.c`
- `sdkconfig.defaults.concierge`: name, greeting, orb colour, concierge defaults, the extra TLS root for dev tunnels, and `AUDIO_PA_IDLE_OFF_MS=60000`. That last line is redundant once PR #14 merges, since it changes the upstream default.
- `components/concierge/certs/microsoft_tls_rsa_root_g2.pem`: added to the CA bundle (see docs/concierge.md)
- `docs/concierge.md`, `UPSTREAM.md`

Every edit to an upstream file is marked `concierge-companion`. Find them with
`git grep -n concierge-companion -- main CMakeLists.txt .github README.md`.
These are the only places a merge can conflict:

| File | What changed | If it conflicts |
| --- | --- | --- |
| `CMakeLists.txt` | `SDKCONFIG_DEFAULTS` adds `sdkconfig.defaults.concierge` | Keep both files in the list. |
| `main/CMakeLists.txt` | `REQUIRES ... concierge` | Re-add the line. |
| `main/agent_prompt.c` | `identity.md` and `boundaries.md` are swapped for the concierge blocks when `CONFIG_CONCIERGE_ENABLE` | Keep the macros. If upstream rewrote those two blocks, decide whether the concierge versions need the same change. |
| `main/dg_agent.c` | Include `concierge.h`; the answer queue (`concierge_reply()`, `concierge_send_pending_answer()`) after `send_function_response()`; `keepalive_task` sends a queued answer first (`answered` gates the other branches); `concierge_on_function_call()` and the `ask_concierge` dispatch at the top of `handle_function_call()`; `concierge_on_text()` plus the `device_line` history skip in `ConversationText`; `concierge_on_user_speaking()` / `concierge_on_agent_done()`; a `FunctionCallCancelled` branch; `concierge_tool_add_definition()` in `send_settings()`; `concierge_tool_init()` at the end of `dg_agent_init()` | Re-apply each hook next to its neighbour. The dispatch has to come before the clock and voice handlers. If upstream reworks `keepalive_task`, keep answers on that task: it's the one TEXT sender `dg_agent_stop()` quiesces. |
| `main/audio_io.c` | Uplink VAD gate: a block whose post-AEC peak is louder than the raw mic can't open the gate | Proposed upstream as [esp32-deepgram-voice-agent#14](https://github.com/dg-edcharbeneau/esp32-deepgram-voice-agent/pull/14), byte-identical. Once it merges, take upstream's version. |
| `main/wifi_prov.c` | `BODY_MAX` 3072, the `cg_fields_t` parser, the parse/save block after `form_get(... "tz" ...)`, and the `cg_err` check | Keep the concierge block before the body is wiped. |
| `main/portal.src.html` | Concierge fields, extra form fields in the POST, and error text shown when the response isn't OK | Re-add the fields and the `cg_*` keys. |
| `.github/workflows/release.yml` | Tag trigger is `companion-v*` | Keep `companion-v*`. |
| `README.md` | Companion banner at the top | Keep the banner. |

### Upstream changes that need a closer look

- **Settings size.** Upstream sessions flapped once the Settings message went
  past about 20 kB (see `HISTORY_REPLAY_BYTES` in `main/dg_agent.c`). The
  concierge blocks and the `ask_concierge` definition together are about 480
  bytes *smaller* than the upstream blocks they replace. Check that margin
  whenever upstream adds functions or prompt text.
- **Function-call handling.** Anything new in `handle_function_call()` or the
  `FunctionCallResponse` path. `ask_concierge` answers from another task,
  seconds after the call.
- **The setup portal and NVS layout.** The companion uses its own NVS
  namespace, `concierge`.
