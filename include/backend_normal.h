#ifndef BACKEND_NORMAL_H
#define BACKEND_NORMAL_H

/* MODEL_TYPE=normal: OpenAI-compatible HTTP API via libcurl (src/backend_normal.c). */

#include "http_client.h"

/* TranslateFn signature (http_client.c dispatch table). */
char *translate_via_openai(OpenAITranslator *translator, const char *from_lang,
                           const char *to_lang, const char *text,
                           const char *request_uuid, const char *timestamp,
                           TranslationError *error, const TranslationControl *control,
                           SourceHint *hint_out);

/* Startup log lines of the normal backend (called by openai_translator_init). */
void normal_backend_init(const Config *config);

#endif /* BACKEND_NORMAL_H */
