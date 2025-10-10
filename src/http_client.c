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
        fprintf(stderr, "Error: Memory allocation failed in curl callback\n");
        return 0;
    }

    response->data = ptr;
    memcpy(&(response->data[response->size]), contents, realsize);
    response->size += realsize;
    response->data[response->size] = '\0';

    return realsize;
}

/* Load prompt template from file */
static char *load_prompt_template(void) {
    const char *template_path = "sample_send.txt";

    /* Resolve path relative to executable */
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        fprintf(stderr, "Warning: Could not determine executable path, using default template\n");
        return strdup("{{PROMPT_PREFIX}} FROM {{LANGUAGE_BASE}} to {{LANGUAGE_TO}} :: {{TEXT}}");
    }
    exe_path[len] = '\0';

    /* Get directory of executable */
    char *exe_dir = strdup(exe_path);
    char *dir = dirname(exe_dir);

    /* Build full path */
    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir, template_path);
    free(exe_dir);

    FILE *fp = fopen(full_path, "r");

    if (!fp) {
        fprintf(stderr, "Warning: Failed to load template from %s, using default\n", full_path);
        return strdup("{{PROMPT_PREFIX}} FROM {{LANGUAGE_BASE}} to {{LANGUAGE_TO}} :: {{TEXT}}");
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buffer = malloc(file_size + 1);
    if (!buffer) {
        fclose(fp);
        return strdup("{{PROMPT_PREFIX}} FROM {{LANGUAGE_BASE}} to {{LANGUAGE_TO}} :: {{TEXT}}");
    }

    size_t read_size = fread(buffer, 1, file_size, fp);
    buffer[read_size] = '\0';
    fclose(fp);

    return buffer;
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

/* Build translation prompt from template */
static char *build_prompt(OpenAITranslator *translator, const char *from_lang,
                         const char *to_lang, const char *text) {
    if (!translator || !from_lang || !to_lang || !text) {
        return NULL;
    }

    const char *from_name = get_language_name(from_lang);
    const char *to_name = get_language_name(to_lang);

    if (!from_name) from_name = from_lang;
    if (!to_name) to_name = to_lang;

    char *prompt = strdup(translator->prompt_template);
    if (!prompt) {
        return NULL;
    }

    /* Replace template variables */
    char *temp;

    temp = str_replace(prompt, "{{PROMPT_PREFIX}}", translator->config->prompt_prefix);
    free(prompt);
    if (!temp) return NULL;
    prompt = temp;

    temp = str_replace(prompt, "{{LANGUAGE_BASE}}", from_name);
    free(prompt);
    if (!temp) return NULL;
    prompt = temp;

    temp = str_replace(prompt, "{{LANGUAGE_TO}}", to_name);
    free(prompt);
    if (!temp) return NULL;
    prompt = temp;

    /* Handle {{TEXT}} replacement or append after :: */
    if (strstr(prompt, "{{TEXT}}")) {
        temp = str_replace(prompt, "{{TEXT}}", text);
        free(prompt);
        if (!temp) return NULL;
        prompt = temp;
    } else {
        /* Check for " :: " pattern */
        char *separator = strstr(prompt, " :: ");
        if (separator) {
            *separator = '\0';
            char *new_prompt = malloc(strlen(prompt) + strlen(text) + 5);
            if (new_prompt) {
                sprintf(new_prompt, "%s :: %s", prompt, text);
                free(prompt);
                prompt = new_prompt;
            }
        }
    }

    return prompt;
}

/* Initialize OpenAI translator */
OpenAITranslator *openai_translator_init(Config *config, int max_retries, int timeout) {
    if (!config) {
        fprintf(stderr, "Error: NULL config\n");
        return NULL;
    }

    OpenAITranslator *translator = calloc(1, sizeof(OpenAITranslator));
    if (!translator) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        return NULL;
    }

    translator->config = config;
    translator->max_retries = max_retries > 0 ? max_retries : DEFAULT_MAX_RETRIES;
    translator->timeout = timeout > 0 ? timeout : DEFAULT_TIMEOUT;
    translator->prompt_template = load_prompt_template();

    if (!translator->prompt_template) {
        free(translator);
        return NULL;
    }

    /* Initialize curl */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    fprintf(stderr, "OpenAI translator initialized: base_url=%s, model=%s\n",
            config->openai_base_url, config->openai_model);

    return translator;
}

/* Free OpenAI translator */
void openai_translator_free(OpenAITranslator *translator) {
    if (!translator) {
        return;
    }

    free(translator->prompt_template);
    free(translator);

    curl_global_cleanup();
}

/* Translate text using OpenAI API */
char *openai_translate(OpenAITranslator *translator, const char *from_lang,
                      const char *to_lang, const char *text,
                      const char *request_uuid, TranslationError *error) {
    if (!translator || !from_lang || !to_lang || !text || !request_uuid) {
        if (error) {
            error->message = strdup("Invalid parameters");
            error->retryable = false;
            error->status_code = 0;
        }
        return NULL;
    }

    char *prompt = build_prompt(translator, from_lang, to_lang, text);
    if (!prompt) {
        if (error) {
            error->message = strdup("Failed to build prompt");
            error->retryable = false;
            error->status_code = 0;
        }
        return NULL;
    }

    fprintf(stderr, "[%s] Starting translation: %s -> %s\n", request_uuid, from_lang, to_lang);

    char *result = NULL;
    int attempt;

    for (attempt = 1; attempt <= translator->max_retries; attempt++) {
        CURL *curl = curl_easy_init();
        if (!curl) {
            fprintf(stderr, "[%s] Failed to initialize curl\n", request_uuid);
            continue;
        }

        /* Build API endpoint URL */
        char url[512];
        snprintf(url, sizeof(url), "%s/chat/completions", translator->config->openai_base_url);

        /* Build JSON request body */
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", translator->config->openai_model);
        cJSON_AddNumberToObject(root, "temperature", 0.3);

        cJSON *messages = cJSON_CreateArray();
        cJSON *message = cJSON_CreateObject();
        cJSON_AddStringToObject(message, "role", "user");
        cJSON_AddStringToObject(message, "content", prompt);
        cJSON_AddItemToArray(messages, message);
        cJSON_AddItemToObject(root, "messages", messages);

        char *json_request = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        /* Setup curl */
        CurlResponse response = {0};
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

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
            fprintf(stderr, "[%s] Curl error (attempt %d/%d): %s\n",
                   request_uuid, attempt, translator->max_retries, curl_easy_strerror(res));
            free(response.data);

            if (attempt < translator->max_retries) {
                int backoff = (int)pow(2, attempt);
                fprintf(stderr, "[%s] Retrying in %d seconds...\n", request_uuid, backoff);
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
            fprintf(stderr, "[%s] Server error %ld (attempt %d/%d)\n",
                   request_uuid, http_code, attempt, translator->max_retries);
            free(response.data);

            if (attempt < translator->max_retries) {
                int backoff = (int)pow(2, attempt);
                fprintf(stderr, "[%s] Retrying in %d seconds...\n", request_uuid, backoff);
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
            fprintf(stderr, "[%s] Client error %ld\n", request_uuid, http_code);
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
            fprintf(stderr, "[%s] Failed to parse response JSON\n", request_uuid);
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
            cJSON *content = cJSON_GetObjectItem(message_obj, "content");

            if (cJSON_IsString(content) && content->valuestring) {
                // Allocate buffers dynamically to handle large translations
                char *unescaped_text = malloc(MAX_TRANSLATION_BUFFER);
                char *cleaned_text = malloc(MAX_CLEANED_TEXT_BUFFER);

                if (!unescaped_text || !cleaned_text) {
                    fprintf(stderr, "[%s] Memory allocation failed for translation buffers\n", request_uuid);
                    free(unescaped_text);
                    free(cleaned_text);
                    cJSON_Delete(response_json);
                    break;
                }

                // First unescape \\n to \n, \\t to \t, etc.
                if (unescape_string(content->valuestring, unescaped_text, MAX_TRANSLATION_BUFFER) != 0) {
                    fprintf(stderr, "[%s] Failed to unescape translation text\n", request_uuid);
                    free(unescaped_text);
                    free(cleaned_text);
                    cJSON_Delete(response_json);
                    break;
                }

                // Then strip emoji and shortcodes
                if (strip_emoji_and_shortcodes(unescaped_text, cleaned_text, MAX_CLEANED_TEXT_BUFFER) != 0) {
                    fprintf(stderr, "[%s] Failed to clean translation text\n", request_uuid);
                    free(unescaped_text);
                    free(cleaned_text);
                    cJSON_Delete(response_json);
                    break;
                }

                result = strdup(cleaned_text);
                free(unescaped_text);
                free(cleaned_text);

                fprintf(stderr, "[%s] Translation completed (attempt %d/%d)\n",
                       request_uuid, attempt, translator->max_retries);
            }
        }

        cJSON_Delete(response_json);

        if (result) {
            break;
        }

        fprintf(stderr, "[%s] No translation in response\n", request_uuid);
        if (error) {
            error->message = strdup("No translation in response");
            error->retryable = false;
            error->status_code = http_code;
        }
        break;
    }

    free(prompt);

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
