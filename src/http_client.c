/**
 * HTTP client module for transbasket.
 * Handles communication with OpenAI-compatible API using libcurl.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <limits.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include "http_client.h"
#include "utils.h"

#define DEFAULT_TIMEOUT 60
#define DEFAULT_MAX_RETRIES 3
#define MAX_TRANSLATION_BUFFER 16384  /* 16KB for unescaped text */
#define MAX_CLEANED_TEXT_BUFFER 8192  /* 8KB for cleaned text */

/* Structure for curl response data */
typedef struct {
    char *data;
    size_t size;
} CurlResponse;

/* Curl write callback */
static size_t transbasket_curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    CurlResponse *response = (CurlResponse *)userp;

    char *ptr = realloc(response->data, response->size + realsize + 1);
    if (!ptr) {
        LOG_DEBUG( "Error: Memory allocation failed in curl callback");
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
                           const char *url, const char *api_key,
                           const char *json_request) {
    if (!timestamp || !uuid || !url || !api_key || !json_request) {
        return;
    }

    /* Create trace directory if it doesn't exist */
    const char *trace_dir = "./trace";
    struct stat st = {0};
    if (stat(trace_dir, &st) == -1) {
        if (mkdir(trace_dir, 0755) == -1) {
            LOG_DEBUG( "Warning: Failed to create trace directory: %s\n", trace_dir);
            return;
        }
    }

    /* Create filename from timestamp and uuid */
    char filename[512];
    snprintf(filename, sizeof(filename), "%s_%s.txt", timestamp, uuid);

    /* Replace ':' with '-' in filename for filesystem compatibility */
    for (char *p = filename; *p; p++) {
        if (*p == ':') *p = '-';
    }

    /* Build full path */
    char filepath[768];
    snprintf(filepath, sizeof(filepath), "%s/%s", trace_dir, filename);

    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        LOG_DEBUG( "Warning: Failed to create debug file: %s\n", filepath);
        return;
    }

    /* Write curl command using heredoc for proper JSON formatting */
    fprintf(fp, "curl -X POST '%s' \\\n", url);
    fprintf(fp, "  -H 'Content-Type: application/json; charset=utf-8' \\");
    fprintf(fp, "  -H 'Authorization: Bearer %s' \\\n", api_key);
    fprintf(fp, "  --fail-with-body -sS \\");
    fprintf(fp, "  --data-binary @- <<'JSON'");
    fprintf(fp, "%s\n", json_request);
    fprintf(fp, "JSON");

    fclose(fp);
    LOG_INFO( "[%s] Debug curl saved to: %s\n", uuid, filepath);
}

/* Replace all occurrences of a substring */
static char *str_replace(const char *orig, const char *rep, const char *with) {
    char *result;
    char *ins;
    char *tmp;
    int len_rep;
    int len_with;
    int len_front;
    int count;

    if (!orig || !rep)
        return NULL;
    len_rep = strlen(rep);
    if (len_rep == 0)
        return NULL;
    if (!with)
        with = "";
    len_with = strlen(with);

    ins = (char *)orig;
    for (count = 0; (tmp = strstr(ins, rep)); ++count) {
        ins = tmp + len_rep;
    }

    tmp = result = malloc(strlen(orig) + (len_with - len_rep) * count + 1);

    if (!result)
        return NULL;

    while (count--) {
        ins = strstr(orig, rep);
        len_front = ins - orig;
        tmp = strncpy(tmp, orig, len_front) + len_front;
        tmp = strcpy(tmp, with) + len_with;
        orig += len_front + len_rep;
    }
    strcpy(tmp, orig);
    return result;
}

/* Build translation instruction message from PROMPT_PREFIX */
static char *build_instruction_message(OpenAITranslator *translator, const char *to_lang) {
    if (!translator || !to_lang) {
        return NULL;
    }

    const char *to_name = get_language_name(to_lang);
    if (!to_name) to_name = to_lang;

    /* Use PROMPT_PREFIX as the instruction message */
    char *instruction = strdup(translator->config->prompt_prefix);
    if (!instruction) {
        return NULL;
    }

    /* Replace [TARGET LANGUAGE] with actual target language name */
    char *temp = str_replace(instruction, "[TARGET LANGUAGE]", to_name);
    if (temp) {
        free(instruction);
        instruction = temp;
    }

    /* Also try replacing {{LANGUAGE_TO}} for backward compatibility */
    temp = str_replace(instruction, "{{LANGUAGE_TO}}", to_name);
    if (temp) {
        free(instruction);
        instruction = temp;
    }

    return instruction;
}

/* Initialize OpenAI translator */
OpenAITranslator *openai_translator_init(Config *config, int max_retries, int timeout) {
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
    translator->max_retries = max_retries > 0 ? max_retries : DEFAULT_MAX_RETRIES;
    translator->timeout = timeout > 0 ? timeout : DEFAULT_TIMEOUT;

    /* Initialize curl */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    LOG_INFO( "OpenAI translator initialized: base_url=%s, model=%s\n",
            config->openai_base_url, config->openai_model);

    return translator;
}

/* Free OpenAI translator */
void openai_translator_free(OpenAITranslator *translator) {
    if (!translator) {
        return;
    }

    free(translator);

    curl_global_cleanup();
}

/* Translate text using OpenAI API */
char *openai_translate(OpenAITranslator *translator, const char *from_lang,
                      const char *to_lang, const char *text,
                      const char *request_uuid, const char *timestamp,
                      TranslationError *error) {
    if (!translator || !from_lang || !to_lang || !text || !request_uuid || !timestamp) {
        if (error) {
            error->message = strdup("Invalid parameters");
            error->retryable = false;
            error->status_code = 0;
        }
        return NULL;
    }

    char *instruction = build_instruction_message(translator, to_lang);
    if (!instruction) {
        if (error) {
            error->message = strdup("Failed to build instruction message");
            error->retryable = false;
            error->status_code = 0;
        }
        return NULL;
    }

    LOG_INFO( "[%s] Starting translation: %s -> %s\n", request_uuid, from_lang, to_lang);

    char *result = NULL;
    int attempt;

    for (attempt = 1; attempt <= translator->max_retries; attempt++) {
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

        /* Message 2: Translation instructions with PROMPT_PREFIX */
        cJSON *instruction_message = cJSON_CreateObject();
        cJSON_AddStringToObject(instruction_message, "role", "user");
        cJSON_AddStringToObject(instruction_message, "content", instruction);
        cJSON_AddItemToArray(messages, instruction_message);

        /* Message 3: Language direction */
        char language_info[256];
        snprintf(language_info, sizeof(language_info), "Translate FROM %s TO %s",
                 get_language_name(from_lang), get_language_name(to_lang));
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

        cJSON_AddItemToObject(root, "messages", messages);

        char *json_request = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        /* Save debug curl command on first attempt if DEBUG enabled */
        if (attempt == 1 && translator->config->debug) {
            save_debug_curl(timestamp, request_uuid, url,
                          translator->config->openai_api_key, json_request);
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
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, translator->timeout);

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

            if (attempt < translator->max_retries) {
                int backoff = (int)pow(2, attempt);
                LOG_DEBUG( "[%s] Retrying in %d seconds...\n", request_uuid, backoff);
                sleep(backoff);
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
                sleep(backoff);
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

        /* Parse response */
        cJSON *response_json = cJSON_Parse(response.data);
        free(response.data);

        if (!response_json) {
            LOG_DEBUG( "[%s] Failed to parse response JSON\n", request_uuid);
            if (error) {
                error->message = strdup("Invalid response JSON");
                error->retryable = false;
                error->status_code = http_code;
            }
            break;
        }

        cJSON *choices = cJSON_GetObjectItem(response_json, "choices");
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
            cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
            cJSON *message_obj = cJSON_GetObjectItem(first_choice, "message");

            /* Check if message object exists */
            if (!message_obj) {
                LOG_DEBUG( "[%s] No message object in response\n", request_uuid);
                result = strdup("nothing contents");
                cJSON_Delete(response_json);
                break;
            }

            cJSON *content = cJSON_GetObjectItem(message_obj, "content");

            /* Check if content exists and is a string */
            if (!cJSON_IsString(content) || !content->valuestring) {
                LOG_DEBUG( "[%s] No content in message object\n", request_uuid);
                result = strdup("nothing contents");
                cJSON_Delete(response_json);
                break;
            }

            // Allocate buffers dynamically to handle large translations
            char *unescaped_text = malloc(MAX_TRANSLATION_BUFFER);
            char *cleaned_text = malloc(MAX_CLEANED_TEXT_BUFFER);

            if (!unescaped_text || !cleaned_text) {
                LOG_DEBUG( "[%s] Memory allocation failed for translation buffers\n", request_uuid);
                free(unescaped_text);
                free(cleaned_text);
                cJSON_Delete(response_json);
                break;
            }

            // First unescape \\n to \n, \\t to \t, etc.
            if (unescape_string(content->valuestring, unescaped_text, MAX_TRANSLATION_BUFFER) != 0) {
                LOG_DEBUG( "[%s] Failed to unescape translation text\n", request_uuid);
                free(unescaped_text);
                free(cleaned_text);
                cJSON_Delete(response_json);
                break;
            }

            // Then strip emoji and shortcodes
            if (strip_emoji_and_shortcodes(unescaped_text, cleaned_text, MAX_CLEANED_TEXT_BUFFER) != 0) {
                LOG_DEBUG( "[%s] Failed to clean translation text\n", request_uuid);
                free(unescaped_text);
                free(cleaned_text);
                cJSON_Delete(response_json);
                break;
            }

            result = strdup(cleaned_text);
            free(unescaped_text);
            free(cleaned_text);

            LOG_DEBUG( "[%s] Translation completed (attempt %d/%d)\n",
                   request_uuid, attempt, translator->max_retries);
        }

        cJSON_Delete(response_json);

        if (result) {
            break;
        }

        LOG_DEBUG( "[%s] No translation in response\n", request_uuid);
        if (error) {
            error->message = strdup("No translation in response");
            error->retryable = false;
            error->status_code = http_code;
        }
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

/* Free translated text */
void free_translated_text(char *text) {
    free(text);
}
