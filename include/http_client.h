#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdbool.h>
#include "slot_gate.h"
#include "config_loader.h"
#include "language_table.h"
#include "translation_control.h"
#include "trans_cache.h"   /* SourceHint */

struct CodexDriver;   /* codex_driver.h */

/* OpenAI translator structure */
typedef struct {
    Config *config;
    LanguageTable *lang_table;  /* Language code/name mappings */
    int max_retries;
    int timeout;

    /* codex backend: limit concurrent codex processes (codex mode only) */
    SlotGate codex_slots;    /* FIFO slots (slot_gate.h), CODEX_MAX_CONCURRENCY */
    bool  codex_slots_inited;
    const struct CodexDriver *codex_driver;  /* how codex is run (codex_driver.h) */
    void *codex_driver_state;                /* the driver's own; read-only after init */
    bool  codex_structured_output;           /* the driver can follow a JSON schema */
} OpenAITranslator;

/* Translation error structure */
typedef struct {
    char *message;
    bool retryable;
    int status_code;
} TranslationError;

/* Initialize OpenAI translator.
 * max_concurrency limits concurrent codex processes (codex mode only). */
OpenAITranslator *openai_translator_init(Config *config, LanguageTable *lang_table,
                                         int max_retries, int timeout, int max_concurrency);

/* Free OpenAI translator */
void openai_translator_free(OpenAITranslator *translator);

/* Translate text with the configured backend (MODEL_TYPE: normal or codex) */
char *openai_translate(
    OpenAITranslator *translator,
    const char *from_lang,
    const char *to_lang,
    const char *text,
    const char *request_uuid,
    const char *timestamp,
    TranslationError *error,
    const TranslationControl *control,
    SourceHint *hint_out          /* may be NULL; set to the model's hint when one was obtained */
);

/* Free translated text */
void free_translated_text(char *text);

#endif /* HTTP_CLIENT_H */
