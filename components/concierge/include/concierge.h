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
 * What the concierge layer needs from the agent client, registered once by
 * dg_agent_init() so this component never includes main/. `say` and `nudge`
 * must copy their text and be safe from any task (dg_agent_announce() and
 * dg_agent_nudge() are).
 */
typedef struct {
    void (*say)(const char *text);   /* InjectAgentMessage, behavior "queue" */
    void (*nudge)(const char *text); /* InjectUserMessage */
    bool (*ready)(void);             /* a session is live */
} concierge_agent_hooks_t;

/*
 * Creates the hold-line state, its one timer and its PSRAM buffer. Call once;
 * later calls are ignored. Without it, ask_concierge still works, with no hold
 * lines and no recovery.
 */
void concierge_tool_init(const concierge_agent_hooks_t *hooks);

/*
 * Conversation events for hold lines and recovery (concierge_hold.h), called
 * from the agent client's event task.
 */
/* Any FunctionCallRequest entry, the device's own functions included. */
void concierge_on_function_call(const char *name);
/* Every ConversationText. Returns true when it is the echo of a line the
 * device injected, which the caller keeps out of the replayed history. */
bool concierge_on_text(const char *role, const char *content);
void concierge_on_user_speaking(void);
void concierge_on_agent_done(void);

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

/*
 * Deepgram's FunctionCallCancelled (sent on barge-in). A call still queued never
 * runs; one in flight finishes its HTTPS turn -- the stay workflow keeps it in
 * the transcript -- but its answer is not sent. Unknown ids are ignored.
 */
void concierge_tool_cancel(const char *id);
