/*
 * Prints the assembled system prompt exactly as send_settings() would put it on
 * the wire, without a device in the loop.
 *
 * The point is iteration speed: a prompt edit is a words change, and waiting on
 * a flash to read the words back is the slowest possible way to review one. The
 * real main/agent_prompt.c is compiled here -- the blocks, the order, the
 * gating and the placeholder expansion are the shipping code, not a copy of it.
 *
 * What is faked is only the ESP-IDF around it (host/prompt_stubs) and the two
 * catalogs, which are tables of strings the assembler never interprets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_name.h"
#include "agent_prompt.h"
#include "faces.h"
#include "orb_colors.h"
#include "voices.h"

/* Stand-ins for the device's saved state. The names are what matter -- the
 * assembler only ever copies them. */
static const voice_t s_hannah = { "hannah", "flux-hannah-en", "American woman, clear and confident", 1 };
static const voice_t s_carmen = { "carmen", "flux-carmen-es", "Spanish woman", 1 };
static const voice_t *s_voice = &s_hannah;

const char *agent_name_get(void) { return "Grammer"; }

const voice_t *voices_find(const char *name) { (void)name; return s_voice; }
const char *voices_current_model(void) { return s_voice->model; }

void voices_listen_model_name(char *out, size_t out_len)
{
    snprintf(out, out_len, "Flux");
}

/* The same sentence main/voices.c writes, for the one voice here that needs it. */
void voices_language_note(char *out, size_t out_len)
{
    snprintf(out, out_len, "%s", s_voice == &s_carmen
             ? " Reply in Spanish unless asked otherwise."
             : "");
}

void voices_describe(char *out, size_t out_len)
{
    snprintf(out, out_len, "hannah, kit, ...");
}
void faces_describe(char *out, size_t out_len)
{
    snprintf(out, out_len, "orb (a breathing sphere of dots), spectrum (...)");
}
void orb_colors_describe(char *out, size_t out_len)
{
    snprintf(out, out_len, "fuchsia (magenta, the default), clay (red), ...");
}

int main(int argc, char **argv)
{
    /* --resumed dumps what a session reopened by a voice change actually sends,
     * which is the one variant nobody remembers to check. */
    int resumed = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--resumed") == 0) {
            resumed = 1;
        } else if (strcmp(argv[i], "--spanish") == 0) {
            /* A non-English voice, to see the {{language}} line land. */
            s_voice = &s_carmen;
        }
    }
    agent_prompt_ctx_t ctx = {
        .notes = resumed ? "You have already been talking with this person for a "
                           "few turns. What follows is that same conversation, not "
                           "a new one, so pick it up where it left off and do not "
                           "start over or greet them again."
                         : NULL,
    };

    char *prompt = agent_prompt_build(&ctx);
    if (prompt == NULL) {
        fprintf(stderr, "agent_prompt_build() returned NULL\n");
        return 1;
    }
    fputs(prompt, stdout);
    fputc('\n', stdout);
    free(prompt);
    return 0;
}
