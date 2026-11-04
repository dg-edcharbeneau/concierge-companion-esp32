/*
 * Hotel Concierge companion: the device keeps its own Deepgram think LLM and
 * local functions, and hands anything about the hotel, the stay, the area or
 * rides to the concierge's public API through one client-side function,
 * ask_concierge.
 *
 * The concierge does not know this device exists. It sees an ordinary API
 * client: POST /api/concierge/sessions to verify the guest, then
 * POST /api/concierge/turns with the returned bearer credential.
 */
#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

#define CONCIERGE_URL_LEN       161
#define CONCIERGE_LAST_NAME_LEN 401 /* MaxNameLength (100 chars) as UTF-8 + NUL */
#define CONCIERGE_ROOM_LEN      5   /* RoomNumberFormat: 1-4 digits + NUL */

/* ---------------- settings (NVS, namespace "concierge") ---------------- */

/*
 * Saves whichever fields are non-empty; blank means keep, as the portal does for
 * the API key. Changing the guest or URL drops the cached credential so the next
 * question re-verifies against the new values.
 */
esp_err_t concierge_store_save(const char *base_url, const char *last_name, const char *room);

/* True when URL, last name and room are all available (NVS or Kconfig seed). */
bool concierge_store_is_configured(void);

/* ---------------- the ask_concierge function ---------------- */

/* Appends the ask_concierge definition to Settings' agent.think.functions. */
void concierge_tool_add_definition(cJSON *functions);

/* Whether a FunctionCallRequest entry is ours. */
bool concierge_tool_is(const char *name);

/*
 * Called once per answer, from the concierge worker task -- not the WebSocket
 * task. `session_tag` is the value passed to concierge_tool_handle_async(), so
 * the caller can drop an answer whose agent connection has since been replaced.
 */
typedef void (*concierge_reply_fn)(const char *session_tag, const char *id,
                                   const char *name, const char *content);

/*
 * Starts answering one ask_concierge call and returns at once: the HTTPS round
 * trip can take many seconds and must not stall the WebSocket event task. Every
 * call gets exactly one reply, including on failure, so the agent never waits
 * on a function result that will not come.
 */
void concierge_tool_handle_async(const char *session_tag, const char *id, const char *name,
                                 const char *arguments_json, concierge_reply_fn reply);
