#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdbool.h>
#include "config_loader.h"

/* OpenAI translator structure */
typedef struct {
    Config *config;
    char *prompt_template;
    int max_retries;
    int timeout;
} OpenAITranslator;

/* Translation error structure */
typedef struct {
    char *message;
    bool retryable;
    int status_code;
} TranslationError;

/* Initialize OpenAI translator */
OpenAITranslator *openai_translator_init(Config *config, int max_retries, int timeout);

/* Free OpenAI translator */
void openai_translator_free(OpenAITranslator *translator);

/* Translate text using OpenAI API */
char *openai_translate(
    OpenAITranslator *translator,
    const char *from_lang,
    const char *to_lang,
    const char *text,
    const char *request_uuid,
    TranslationError *error
);

/* Free translated text */
void free_translated_text(char *text);

#endif /* HTTP_CLIENT_H */
