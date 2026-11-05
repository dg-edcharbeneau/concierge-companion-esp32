# The Hotel Concierge companion layer. Kept as its own component, outside main/,
# so an upstream merge from esp32-deepgram-voice-agent cannot conflict with it --
# see UPSTREAM.md for the few hook lines that do live in upstream files.
idf_component_register(
    SRCS
        concierge_store.c
        concierge_client.c
        concierge_tool.c
        concierge_hold.c
    INCLUDE_DIRS include
    # Replacements for main/prompt/identity.md and boundaries.md, picked in
    # main/agent_prompt.c. The embed symbols are global, so main can name them.
    EMBED_TXTFILES
        prompt/concierge_identity.md
        prompt/concierge_boundaries.md
    REQUIRES
        json
        esp_http_client
        mbedtls
        nvs_flash
        esp_timer
)
