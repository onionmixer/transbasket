/**
 * normal backend: OpenAI-compatible HTTP API via libcurl.
 * Split out of http_client.c (PLAN_BACKEND_MODULES.md step 1); the code below is moved verbatim.
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
#include "prompt_builder.h"
#include "trace_file.h"

/* Upper bound on a normal-backend reply body, as for codex (CODEX_OUT_MAX). */
#define MAX_UPSTREAM_RESPONSE (4 * 1024 * 1024)

/* Structure for curl response data */
typedef struct {
    char *data;
    size_t size;
    bool too_large;      /* body passed MAX_UPSTREAM_RESPONSE: 502, not retried */
    bool alloc_failed;   /* realloc failed in the write callback: not retried */
} CurlResponse;

/* Curl calls this during transfer and while waiting for network progress. */
static int translation_progress(void *context, curl_off_t total_down, curl_off_t down,
                                curl_off_t total_up, curl_off_t up) {
    (void)total_down; (void)down; (void)total_up; (void)up;
    return translation_remaining_ms(context) == 0;
}

/* Curl write callback */
static size_t transbasket_curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    CurlResponse *response = (CurlResponse *)userp;
    if (nmemb && size > SIZE_MAX / nmemb) {
        response->too_large = true;
        return 0;
    }
    size_t realsize = size * nmemb;
    if (realsize > MAX_UPSTREAM_RESPONSE || response->size > MAX_UPSTREAM_RESPONSE - realsize) {
        response->too_large = true;
        return 0;
    }

    char *ptr = realloc(response->data, response->size + realsize + 1);
    if (!ptr) {
        LOG_DEBUG( "Error: Memory allocation failed in curl callback");
        response->alloc_failed = true;
        return 0;
    }

    response->data = ptr;
    memcpy(&(response->data[response->size]), contents, realsize);
    response->size += realsize;
    response->data[response->size] = '\0';

    return realsize;
}

/* Save debug curl command to file */
static void save_debug_curl(const char *timestamp, const char *uuid,
                           const char *url, const char *json_request) {
    if (!timestamp || !uuid || !url || !json_request) {
        return;
    }

    char base[512];
    trace_base(base, sizeof(base), timestamp, uuid);
    char filepath[768];
    FILE *fp = open_trace_file(base, ".txt", filepath, sizeof(filepath));
    if (!fp) {
        LOG_DEBUG( "Warning: Failed to create debug file for %s\n", uuid);
        return;
    }

    /* Write curl command using heredoc for proper JSON formatting */
    fprintf(fp, "curl -X POST '%s' \\\n", url);
    fprintf(fp, "  -H 'Content-Type: application/json; charset=utf-8' \\");
    fprintf(fp, "  -H 'Authorization: Bearer <redacted>' \\\n");   /* never the real key */
    fprintf(fp, "  --fail-with-body -sS \\");
    fprintf(fp, "  --data-binary @- <<'JSON'\n");
    fprintf(fp, "%s\n", json_request);
    fprintf(fp, "JSON");

    fclose(fp);
    LOG_INFO( "[%s] Debug curl saved to: %s\n", uuid, filepath);
}

/* Parse SSE (Server-Sent Events) chunk and extract content from delta */
/* Outcome of reading an SSE reply (PLAN_OPEN_ISSUES.md D/E). Anything but SSE_OK
 * fails the whole reply (502, not cached, not retried): returning the events that
 * did parse would cache a translation with a hole in it. */
typedef enum {
    SSE_OK = 0,
    SSE_NUL_ESCAPE,    /* an escape cJSON decodes to U+0000 */
    SSE_RAW_NUL,       /* a NUL byte anywhere in the body */
    SSE_BAD_JSON,      /* event data that is not exactly one JSON value */
    SSE_ERROR_EVENT,   /* "event: error", or a JSON "error" member */
    SSE_BAD_CONTENT,   /* delta.content neither a string nor null */
    SSE_NO_MEMORY
} SseStatus;

static const char *sse_status_message(SseStatus st) {
    switch (st) {
    case SSE_NUL_ESCAPE: return "Upstream response contains U+0000";
    case SSE_RAW_NUL: return "Upstream response contains a NUL byte";
    case SSE_BAD_JSON: return "Malformed event in upstream stream";
    case SSE_ERROR_EVENT: return "Upstream stream reported an error";
    case SSE_BAD_CONTENT: return "Unexpected content type in upstream stream";
    case SSE_NO_MEMORY: return "Memory allocation failed";
    default: return "Upstream stream error";
    }
}

/* One event's data. "No content" (role-only first chunk, [DONE], usage chunk with
 * empty choices, finish chunk with an empty delta or content:null, any JSON without
 * choices) adds nothing; a content string is appended to acc, whose capacity is the
 * whole body + 1 - decoded JSON strings are never longer than their source bytes. */
static SseStatus sse_event(const char *data, size_t len, bool error_event,
                           char *acc, size_t *acc_len, size_t acc_cap) {
    if (error_event) {
        return SSE_ERROR_EVENT;
    }
    if (len == 6 && memcmp(data, "[DONE]", 6) == 0) {
        return SSE_OK;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        return SSE_NO_MEMORY;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';

    const char *end = NULL;
    cJSON *json = cJSON_ParseWithOpts(buf, &end, 0);
    if (!json) {
        free(buf);
        return SSE_BAD_JSON;
    }
    for (const char *q = end; *q; q++) {
        if (*q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') {
            cJSON_Delete(json);
            free(buf);
            return SSE_BAD_JSON;   /* bytes after the JSON value */
        }
    }
    bool nul = json_decodes_nul((const unsigned char *)buf, (size_t)(end - buf));
    free(buf);
    if (nul) {
        cJSON_Delete(json);
        return SSE_NUL_ESCAPE;
    }

    SseStatus st = SSE_OK;
    cJSON *err = cJSON_GetObjectItem(json, "error");
    if (err && !cJSON_IsNull(err)) {
        st = SSE_ERROR_EVENT;
    } else {
        cJSON *choices = cJSON_GetObjectItem(json, "choices");
        cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *delta = first ? cJSON_GetObjectItem(first, "delta") : NULL;
        cJSON *content = delta ? cJSON_GetObjectItem(delta, "content") : NULL;
        if (content && !cJSON_IsNull(content)) {
            if (!cJSON_IsString(content) || !content->valuestring) {
                st = SSE_BAD_CONTENT;
            } else {
                size_t n = strlen(content->valuestring);
                if (n >= acc_cap - *acc_len) {
                    st = SSE_NO_MEMORY;   /* cannot happen: see the capacity note above */
                } else {
                    memcpy(acc + *acc_len, content->valuestring, n);
                    *acc_len += n;
                    acc[*acc_len] = '\0';
                }
            }
        }
    }
    cJSON_Delete(json);
    return st;
}

/* One SSE field line (not blank, not a comment): the name runs to the first ':' or
 * the end of the line, the value follows the colon minus one leading space; a line
 * without a colon is a field with an empty value. Only data and event matter. */
static void sse_field(const char *line, size_t len, char *ev, size_t *ev_len, bool *ev_data, bool *ev_error) {
    const char *colon = memchr(line, ':', len);
    size_t name_len = colon ? (size_t)(colon - line) : len;
    const char *val = colon ? colon + 1 : line + len;
    size_t val_len = colon ? len - name_len - 1 : 0;
    if (val_len > 0 && val[0] == ' ') {
        val++;
        val_len--;
    }
    if (name_len == 4 && memcmp(line, "data", 4) == 0) {
        if (*ev_data) ev[(*ev_len)++] = '\n';
        memcpy(ev + *ev_len, val, val_len);
        *ev_len += val_len;
        *ev_data = true;
    } else if (name_len == 5 && memcmp(line, "event", 5) == 0) {
        *ev_error = val_len == 5 && memcmp(val, "error", 5) == 0;
    }
}

/* Handle a streaming response: an SSE body of `size` bytes (not a C string - a NUL
 * byte must not end the scan early). A leading UTF-8 BOM is skipped; lines end in LF,
 * CRLF or CR; a blank line ends an event; "data" lines are joined with LF; ":"
 * comment lines and other fields are ignored. A last event without the blank line is
 * still read (deliberately, unlike the SSE rule: dropping it would return the reply
 * without its end as a success). Returns the concatenated content, or NULL with
 * *status set. */
static char *handle_streaming_response(const char *data, size_t size, const char *request_uuid,
                                       SseStatus *status) {
    (void)request_uuid;
    *status = SSE_OK;
    if (!data) {
        return NULL;
    }
    if (memchr(data, 0, size)) {
        *status = SSE_RAW_NUL;
        return NULL;
    }
    char *acc = malloc(size + 1);
    char *ev = malloc(size + 1);
    if (!acc || !ev) {
        free(acc);
        free(ev);
        *status = SSE_NO_MEMORY;
        return NULL;
    }
    acc[0] = '\0';
    size_t acc_len = 0, ev_len = 0;
    bool ev_data = false, ev_error = false;
    size_t i = (size >= 3 && memcmp(data, "\xEF\xBB\xBF", 3) == 0) ? 3 : 0;

    for (;;) {
        size_t j = i;
        while (j < size && data[j] != '\n' && data[j] != '\r') {
            j++;
        }
        bool last = j >= size;
        const char *line = data + i;
        size_t line_len = j - i;

        if (line_len > 0 && line[0] != ':') {
            sse_field(line, line_len, ev, &ev_len, &ev_data, &ev_error);
        }
        if (line_len == 0 || last) {
            /* an event whose data is empty is not dispatched (SSE; also what the old code did) */
            if ((ev_data && ev_len > 0) || ev_error) {
                SseStatus st = sse_event(ev, ev_len, ev_error, acc, &acc_len, size + 1);
                if (st != SSE_OK) {
                    *status = st;
                    free(acc);
                    free(ev);
                    return NULL;
                }
            }
            ev_len = 0;
            ev_data = ev_error = false;
        }

        if (last) {
            break;
        }
        i = (data[j] == '\r' && j + 1 < size && data[j + 1] == '\n') ? j + 2 : j + 1;
    }
    free(ev);

    if (acc_len == 0) {
        free(acc);
        return NULL;
    }
    return acc;
}

/* Handle non-streaming response - extract from message.content */
static char *handle_non_streaming_response(const char *response_data, const char *request_uuid, bool *nul_out) {
    if (nul_out) *nul_out = false;
    if (!response_data || !request_uuid) {
        return NULL;
    }

    const char *parse_end = NULL;
    cJSON *response_json = cJSON_ParseWithOpts(response_data, &parse_end, 0);
    if (!response_json) {
        LOG_DEBUG("[%s] Failed to parse response JSON\n", request_uuid);
        return NULL;
    }
    if (json_decodes_nul((const unsigned char *)response_data, (size_t)(parse_end - response_data))) {
        if (nul_out) *nul_out = true;
        cJSON_Delete(response_json);
        return NULL;
    }

    char *result = NULL;
    cJSON *choices = cJSON_GetObjectItem(response_json, "choices");

    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
        cJSON *message_obj = cJSON_GetObjectItem(first_choice, "message");

        /* Check if message object exists */
        if (!message_obj) {
            LOG_DEBUG("[%s] No message object in response\n", request_uuid);
            cJSON_Delete(response_json);
            return result;
        }

        cJSON *content = cJSON_GetObjectItem(message_obj, "content");

        /* Check if content exists and is a string */
        if (!cJSON_IsString(content) || !content->valuestring) {
            LOG_DEBUG("[%s] No content in message object\n", request_uuid);
            cJSON_Delete(response_json);
            return result;
        }

        result = strdup(content->valuestring);
    } else {
        LOG_DEBUG("[%s] No choices in response\n", request_uuid);
    }

    cJSON_Delete(response_json);
    return result;
}

/* Translate text using OpenAI-compatible HTTP API (normal backend) */
char *translate_via_openai(OpenAITranslator *translator, const char *from_lang,
                      const char *to_lang, const char *text,
                      const char *request_uuid, const char *timestamp,
                      TranslationError *error, const TranslationControl *control,
                      SourceHint *hint_out) {
    (void)hint_out;   /* no model hint from the normal backend (plan step 6, deferred) */
    if (!translator || !from_lang || !to_lang || !text || !request_uuid || !timestamp) {
        if (error) {
            error->message = strdup("Invalid parameters");
            error->retryable = false;
            error->status_code = 0;
        }
        return NULL;
    }

    /* Build instruction message with all placeholders replaced */
    bool use_single_message = false;
    char *instruction = build_instruction_message(translator, from_lang, to_lang, text, &use_single_message, false);
    if (!instruction) {
        if (error) {
            error->message = strdup("Failed to build instruction message");
            error->retryable = false;
            error->status_code = 0;
        }
        return NULL;
    }

    LOG_INFO( "[%s] Starting translation: %s -> %s (format: %s)\n",
            request_uuid, from_lang, to_lang,
            use_single_message ? "TranslateGemma" : "legacy");

    char *result = NULL;
    int attempt;

    for (attempt = 1; attempt <= translator->max_retries; attempt++) {
        if (!translation_remaining_ms(control)) break;
        CURL *curl = curl_easy_init();
        if (!curl) {
            LOG_DEBUG( "[%s] Failed to initialize curl\n", request_uuid);
            continue;
        }

        /* Build API endpoint URL */
        char url[512];
        snprintf(url, sizeof(url), "%s/chat/completions", translator->config->openai_base_url);

        /* Build JSON request body */
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", translator->config->openai_model);

        /* Add reasoning object (always included) */
        cJSON *reasoning = cJSON_CreateObject();
        cJSON_AddStringToObject(reasoning, "effort", translator->config->reasoning_effort);
        cJSON_AddItemToObject(root, "reasoning", reasoning);

        cJSON_AddNumberToObject(root, "temperature", translator->config->temperature);
        cJSON_AddNumberToObject(root, "top_p", translator->config->top_p);
        cJSON_AddNumberToObject(root, "seed", translator->config->seed);
        cJSON_AddBoolToObject(root, "stream", translator->config->stream);
        cJSON_AddNumberToObject(root, "frequency_penalty", translator->config->frequency_penalty);
        cJSON_AddNumberToObject(root, "presence_penalty", translator->config->presence_penalty);

        cJSON *messages = cJSON_CreateArray();

        /* Message 1: System role */
        cJSON *system_message = cJSON_CreateObject();
        cJSON_AddStringToObject(system_message, "role", "system");
        cJSON_AddStringToObject(system_message, "content", translator->config->system_role);
        cJSON_AddItemToArray(messages, system_message);

        if (use_single_message) {
            /* TranslateGemma format: Single user message with all content */
            /* The instruction already contains the text via {TEXT} placeholder */
            cJSON *instruction_message = cJSON_CreateObject();
            cJSON_AddStringToObject(instruction_message, "role", "user");
            cJSON_AddStringToObject(instruction_message, "content", instruction);
            cJSON_AddItemToArray(messages, instruction_message);
        } else {
            /* Legacy format: Multiple messages */
            /* Message 2: Translation instructions with PROMPT_PREFIX */
            cJSON *instruction_message = cJSON_CreateObject();
            cJSON_AddStringToObject(instruction_message, "role", "user");
            cJSON_AddStringToObject(instruction_message, "content", instruction);
            cJSON_AddItemToArray(messages, instruction_message);

            /* Message 3: Language direction.
             * get_language_name() returns NULL for codes that have no entry in
             * LANGUAGE_NAMES (e.g. "swe"), and passing NULL to %s is undefined
             * behaviour - fall back to the raw code. */
            bool auto_source = is_auto_source_language(from_lang);
            const char *from_name = auto_source ? NULL : get_language_name(from_lang);
            const char *to_name_dir = get_language_name(to_lang);
            if (!from_name) from_name = from_lang;
            if (!to_name_dir) to_name_dir = to_lang;

            char language_info[256];
            if (auto_source) {
                snprintf(language_info, sizeof(language_info),
                         "Determine the source language from the text and translate TO %s", to_name_dir);
            } else {
                snprintf(language_info, sizeof(language_info), "Translate FROM %s TO %s",
                         from_name, to_name_dir);
            }
            cJSON *language_message = cJSON_CreateObject();
            cJSON_AddStringToObject(language_message, "role", "user");
            cJSON_AddStringToObject(language_message, "content", language_info);
            cJSON_AddItemToArray(messages, language_message);

            /* Message 4: Actual text to translate */
            /* cJSON automatically escapes newlines (\n) and other special characters */
            /* Wrap text in <source> tags */
            size_t wrapped_text_len = strlen(text) + strlen("<source></source>") + 1;
            char *wrapped_text = malloc(wrapped_text_len);
            if (!wrapped_text) {
                LOG_DEBUG( "[%s] Failed to allocate memory for wrapped text\n", request_uuid);
                cJSON_Delete(root);
                free(instruction);
                if (error) {
                    error->message = strdup("Memory allocation failed");
                    error->retryable = false;
                    error->status_code = 0;
                }
                return NULL;
            }
            snprintf(wrapped_text, wrapped_text_len, "<source>%s</source>", text);

            cJSON *text_message = cJSON_CreateObject();
            cJSON_AddStringToObject(text_message, "role", "user");
            cJSON_AddStringToObject(text_message, "content", wrapped_text);
            cJSON_AddItemToArray(messages, text_message);

            free(wrapped_text);
        }

        cJSON_AddItemToObject(root, "messages", messages);

        char *json_request = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        /* Save debug curl command on first attempt if DEBUG enabled */
        if (attempt == 1 && translator->config->debug) {
            save_debug_curl(timestamp, request_uuid, url, json_request);
        }

        /* Setup curl */
        CurlResponse response = {0};
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

        char auth_header[512];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
                translator->config->openai_api_key);
        headers = curl_slist_append(headers, auth_header);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_request);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, transbasket_curl_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        long timeout_ms = translation_remaining_ms(control);
        if (timeout_ms > translator->timeout * 1000L) timeout_ms = translator->timeout * 1000L;
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, translation_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, control);

        /* Perform request */
        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        free(json_request);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            LOG_DEBUG( "[%s] Curl error (attempt %d/%d): %s\n",
                   request_uuid, attempt, translator->max_retries, curl_easy_strerror(res));
            free(response.data);

            if (response.too_large || response.alloc_failed) {
                /* our own limit or memory, not the network: retrying cannot help */
                LOG_INFO("[%s] %s\n", request_uuid, response.too_large
                         ? "upstream response larger than 4 MB" : "memory allocation failed reading the upstream response");
                if (error) {
                    error->message = strdup(response.too_large ? "Upstream response too large" : "Memory allocation failed");
                    error->retryable = false;
                    error->status_code = 502;
                }
                break;
            }

            if (attempt < translator->max_retries) {
                int backoff = (int)pow(2, attempt);
                LOG_DEBUG( "[%s] Retrying in %d seconds...\n", request_uuid, backoff);
                if (!translation_pause(control, backoff * 1000L)) break;
                continue;
            }

            if (error) {
                error->message = strdup(curl_easy_strerror(res));
                error->retryable = true;
                error->status_code = 0;
            }
            break;
        }

        /* Check HTTP status */
        if (http_code >= 500) {
            LOG_DEBUG( "[%s] Server error %ld (attempt %d/%d)\n",
                   request_uuid, http_code, attempt, translator->max_retries);
            free(response.data);

            if (attempt < translator->max_retries) {
                int backoff = (int)pow(2, attempt);
                LOG_DEBUG( "[%s] Retrying in %d seconds...\n", request_uuid, backoff);
                if (!translation_pause(control, backoff * 1000L)) break;
                continue;
            }

            if (error) {
                error->message = strdup("Server error");
                error->retryable = true;
                error->status_code = http_code;
            }
            break;
        }

        if (http_code >= 400) {
            LOG_DEBUG( "[%s] Client error %ld\n", request_uuid, http_code);
            free(response.data);

            if (error) {
                error->message = strdup("Client error");
                error->retryable = false;
                error->status_code = http_code;
            }
            break;
        }

        /* Parse response based on streaming mode */
        char *raw_translation = NULL;
        bool upstream_nul = false;
        SseStatus sse = SSE_OK;

        if (translator->config->stream) {
            /* Handle streaming response */
            raw_translation = handle_streaming_response(response.data, response.size, request_uuid, &sse);
            upstream_nul = sse == SSE_NUL_ESCAPE;
        } else {
            /* Handle non-streaming response */
            raw_translation = handle_non_streaming_response(response.data, request_uuid, &upstream_nul);
        }

        free(response.data);

        if (sse != SSE_OK && sse != SSE_NUL_ESCAPE) {
            free(raw_translation);
            LOG_INFO("[%s] %s\n", request_uuid, sse_status_message(sse));
            if (error) {
                error->message = strdup(sse_status_message(sse));
                error->retryable = false;
                error->status_code = 502;
            }
            break;
        }

        if (upstream_nul) {
            /* cJSON would have cut the reply at a decoded NUL; never cache that. */
            free(raw_translation);
            LOG_INFO("[%s] upstream response contains an escape that decodes to U+0000\n", request_uuid);
            if (error) {
                error->message = strdup("Upstream response contains U+0000");
                error->retryable = false;
                error->status_code = 502;
            }
            break;
        }

        if (!raw_translation) {
            LOG_DEBUG("[%s] Failed to extract translation from response\n", request_uuid);
            if (error) {
                error->message = strdup("No translation in response");
                error->retryable = false;
                error->status_code = http_code;
            }
            break;
        }

        /* Process the raw translation: unescape and clean */
        /* Neither step writes more than it reads (PLAN_OPEN_ISSUES.md C), so input
         * length + 1 always fits: no 16 KB / 8 KB caps, no silent truncation. */
        size_t raw_len = strlen(raw_translation);
        char *unescaped_text = malloc(raw_len + 1);
        char *cleaned_text = malloc(raw_len + 1);

        if (!unescaped_text || !cleaned_text) {
            LOG_DEBUG("[%s] Memory allocation failed for translation buffers\n", request_uuid);
            free(unescaped_text);
            free(cleaned_text);
            free(raw_translation);
            if (error) {
                error->message = strdup("Memory allocation failed");
                error->retryable = false;
                error->status_code = 0;
            }
            break;
        }

        /* First unescape \\n to \n, \\t to \t, etc. */
        if (unescape_string(raw_translation, unescaped_text, raw_len + 1) != 0) {
            LOG_DEBUG("[%s] Failed to unescape translation text\n", request_uuid);
            free(unescaped_text);
            free(cleaned_text);
            free(raw_translation);
            if (error) {
                error->message = strdup("Failed to process translation text");
                error->retryable = false;
                error->status_code = 0;
            }
            break;
        }

        /* Then strip emoji and shortcodes */
        if (strip_emoji_and_shortcodes(unescaped_text, cleaned_text, raw_len + 1) != 0) {
            LOG_DEBUG("[%s] Failed to clean translation text\n", request_uuid);
            free(unescaped_text);
            free(cleaned_text);
            free(raw_translation);
            if (error) {
                error->message = strdup("Failed to clean translation text");
                error->retryable = false;
                error->status_code = 0;
            }
            break;
        }

        result = strdup(cleaned_text);
        free(unescaped_text);
        free(cleaned_text);
        free(raw_translation);

        if (result && !*result) {
            free(result);
            result = NULL;
            if (error) {
                error->message = strdup("Translation returned empty content");
                error->retryable = false;
                error->status_code = 0;
            }
            break;
        }

        LOG_INFO("[%s] Translation completed (attempt %d/%d, mode: %s)\n",
               request_uuid, attempt, translator->max_retries,
               translator->config->stream ? "streaming" : "non-streaming");

        break;
    }

    free(instruction);

    if (!result && error && !error->message) {
        error->message = strdup("Translation failed after all retries");
        error->retryable = true;
        error->status_code = 0;
    }

    return result;
}

/* MODEL_TYPE=normal part of openai_translator_init(): its startup log lines. */
void normal_backend_init(const Config *config) {
    LOG_INFO( "OpenAI translator initialized: base_url=%s, model=%s\n",
            config->openai_base_url, config->openai_model);
    if (config->source_hint_mode == SOURCE_HINT_MODEL) {
        LOG_INFO("Warning: SOURCE_HINT_MODE=\"model\" is only implemented for MODEL_TYPE=\"codex\"; "
                "source hints come from the script classifier\n");
    }
}
