#ifndef BACKEND_CODEX_H
#define BACKEND_CODEX_H

/* MODEL_TYPE=codex (src/backend_codex.c): one run of the codex driver per request (codex_driver.h). */

#include "http_client.h"

/* TranslateFn signature (http_client.c dispatch table). */
char *translate_via_codex(OpenAITranslator *translator, const char *from_lang,
                          const char *to_lang, const char *text,
                          const char *request_uuid, const char *timestamp,
                          TranslationError *error, const TranslationControl *control,
                          SourceHint *hint_out);

/* Concurrency semaphore, then the codex driver's init (CLI probe and its startup lines).
 * 0, or -1 when the semaphore or the driver state cannot be created; the semaphore is
 * destroyed again here and the caller then cleans up curl and frees the translator. */
int codex_backend_init(OpenAITranslator *translator, const Config *config, int max_concurrency);

/* Frees the driver state and destroys the semaphore, if codex_backend_init() created them. */
void codex_backend_free(OpenAITranslator *translator);

#endif /* BACKEND_CODEX_H */
