/**
 * HTTP client module for transbasket.
 * Handles communication with OpenAI-compatible API using libcurl.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <math.h>
#include <limits.h>
#include <libgen.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <stdint.h>
#include <stdatomic.h>
#include "http_client.h"
#include "codex_exec.h"
#include "strict_json.h"
#include "source_hint.h"
#include "language_table.h"
#include "utils.h"
#include "backend_normal.h"
#include "backend_codex.h"

#define DEFAULT_TIMEOUT 60
#define DEFAULT_MAX_RETRIES 3

/* Initialize OpenAI translator */
OpenAITranslator *openai_translator_init(Config *config, LanguageTable *lang_table,
                                         int max_retries, int timeout, int max_concurrency) {
    if (!config) {
        LOG_DEBUG( "Error: NULL config");
        return NULL;
    }

    OpenAITranslator *translator = calloc(1, sizeof(OpenAITranslator));
    if (!translator) {
        LOG_DEBUG( "Error: Memory allocation failed");
        return NULL;
    }

    translator->config = config;
    translator->lang_table = lang_table;
    translator->max_retries = max_retries > 0 ? max_retries : DEFAULT_MAX_RETRIES;
    translator->timeout = timeout > 0 ? timeout : DEFAULT_TIMEOUT;
    translator->codex_slots_inited = false;

    /* Initialize curl */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (config->model_type == MODEL_TYPE_CODEX) {
        if (codex_backend_init(translator, config, max_concurrency) != 0) {
            curl_global_cleanup();
            free(translator);
            return NULL;
        }
    } else {
        normal_backend_init(config);
    }

    return translator;
}

/* Free OpenAI translator */
void openai_translator_free(OpenAITranslator *translator) {
    if (!translator) {
        return;
    }

    codex_backend_free(translator);

    free(translator);

    curl_global_cleanup();
}

/* Translation backend dispatch table.
 * To add a backend: implement a `translate_via_X()` with this exact signature,
 * add a ModelType enum value, and add one row here. No other dispatch edits. */
typedef char *(*TranslateFn)(OpenAITranslator *, const char *, const char *,
                             const char *, const char *, const char *,
                             TranslationError *, const TranslationControl *, SourceHint *);

typedef struct {
    ModelType   type;
    const char *name;
    TranslateFn translate;
} TranslationBackend;

static const TranslationBackend TRANSLATION_BACKENDS[] = {
    { MODEL_TYPE_NORMAL, "normal", translate_via_openai },
    { MODEL_TYPE_CODEX,  "codex",  translate_via_codex  },
};

/* Translate text using the configured backend (dispatch on MODEL_TYPE). */
char *openai_translate(OpenAITranslator *translator, const char *from_lang,
                       const char *to_lang, const char *text,
                       const char *request_uuid, const char *timestamp,
                       TranslationError *error, const TranslationControl *control,
                       SourceHint *hint_out) {
    if (hint_out) memset(hint_out, 0, sizeof(*hint_out));
    if (!translator || !translator->config) {
        if (error) { error->message = strdup("Invalid translator"); error->retryable = false; error->status_code = 0; }
        return NULL;
    }

    ModelType mt = translator->config->model_type;
    TranslateFn translate = translate_via_openai;
    for (size_t i = 0; i < sizeof(TRANSLATION_BACKENDS) / sizeof(TRANSLATION_BACKENDS[0]); i++) {
        if (TRANSLATION_BACKENDS[i].type == mt) { translate = TRANSLATION_BACKENDS[i].translate; break; }
    }
    char *result = NULL;
    if (translation_remaining_ms(control))
        result = translate(translator, from_lang, to_lang, text, request_uuid, timestamp, error, control, hint_out);
    if (!translation_remaining_ms(control)) {
        free(result);
        result = NULL;
        if (hint_out) memset(hint_out, 0, sizeof(*hint_out));
        if (error) {
            free(error->message);
            bool cancelled = translation_cancelled(control);
            error->message = strdup(cancelled ? "Server shutting down" : "Translation deadline exceeded");
            error->retryable = true;
            error->status_code = cancelled || mt == MODEL_TYPE_NORMAL ? 503 : 504;
        }
    }
    return result;
}

/* Free translated text */
void free_translated_text(char *text) {
    free(text);
}
