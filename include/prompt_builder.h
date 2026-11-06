#ifndef PROMPT_BUILDER_H
#define PROMPT_BUILDER_H

/* Prompt building shared by the normal and codex backends (src/prompt_builder.c). */

#include <stdbool.h>
#include "http_client.h"

/* Replace all occurrences of `rep` in `orig` with `with` (caller frees). */
char *str_replace(const char *orig, const char *rep, const char *with);

/* The translation instruction from PROMPT_PREFIX_FILE (or the built-in auto policy);
 * see the comment above the definition. Caller frees. */
char *build_instruction_message(OpenAITranslator *translator,
                                const char *from_lang, const char *to_lang,
                                const char *text, bool *is_single_message, bool structured);

#endif /* PROMPT_BUILDER_H */
