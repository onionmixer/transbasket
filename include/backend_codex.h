#ifndef BACKEND_CODEX_H
#define BACKEND_CODEX_H

/* MODEL_TYPE=codex: one `codex exec` run per request (src/backend_codex.c). */

#include "http_client.h"

/* TranslateFn signature (http_client.c dispatch table). */
char *translate_via_codex(OpenAITranslator *translator, const char *from_lang,
                          const char *to_lang, const char *text,
                          const char *request_uuid, const char *timestamp,
                          TranslationError *error, const TranslationControl *control,
                          SourceHint *hint_out);

/* Concurrency semaphore, CLI probe and startup log lines. 0, or -1 when the semaphore
 * cannot be created (the caller then cleans up curl and frees the translator). */
int codex_backend_init(OpenAITranslator *translator, const Config *config, int max_concurrency);

/* Destroys the semaphore if codex_backend_init() created it. */
void codex_backend_free(OpenAITranslator *translator);

#endif /* BACKEND_CODEX_H */
