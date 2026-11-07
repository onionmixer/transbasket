/**
 * JSON handler module for transbasket.
 * Handles JSON request/response parsing and generation using cJSON.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include "json_handler.h"
#include "utils.h"
#include "strict_json.h"

#define MAX_TEXT_LENGTH 10000
#define MIN_TEXT_LENGTH 1

/* Parse translation request from JSON string */
TranslationRequest *parse_translation_request(const char *json_str) {
    if (!json_str) {
        fprintf(stderr, "Error: NULL JSON string\n");
        return NULL;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithOpts(json_str, &parse_end, 0);   /* == cJSON_Parse, plus the end */
    if (!root) {
        /* the offset only: the body is the client's, and cJSON_GetErrorPtr() is global state */
        LOG_INFO("Error: Failed to parse JSON at byte %td", parse_end ? parse_end - json_str : (ptrdiff_t)-1);
        return NULL;
    }

    /* cJSON would have cut some field (text, uuid, a key, ...) at a decoded NUL:
     * the request is not what the client sent, so reject it (422). */
    if (json_decodes_nul((const unsigned char *)json_str, (size_t)(parse_end - json_str))) {
        fprintf(stderr, "Error: request contains an escape that decodes to U+0000\n");
        cJSON_Delete(root);
        return NULL;
    }

    TranslationRequest *req = calloc(1, sizeof(TranslationRequest));
    if (!req) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        cJSON_Delete(root);
        return NULL;
    }

    /* Parse timestamp */
    cJSON *timestamp = cJSON_GetObjectItem(root, "timestamp");
    if (!cJSON_IsString(timestamp) || !timestamp->valuestring) {
        fprintf(stderr, "Error: Missing or invalid 'timestamp' field\n");
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    if (!validate_timestamp(timestamp->valuestring)) {
        char esc[300];
        log_escape(timestamp->valuestring, 64, esc, sizeof(esc));
        LOG_INFO("Error: Invalid timestamp format: %s", esc);
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    /* validate_timestamp() accepts at most TIMESTAMP_MAX_LEN bytes, so this never cuts */
    _Static_assert(sizeof(req->timestamp) == TIMESTAMP_MAX_LEN + 1, "timestamp buffer");
    strncpy(req->timestamp, timestamp->valuestring, sizeof(req->timestamp) - 1);

    /* Parse UUID */
    cJSON *uuid = cJSON_GetObjectItem(root, "uuid");
    if (!cJSON_IsString(uuid) || !uuid->valuestring) {
        fprintf(stderr, "Error: Missing or invalid 'uuid' field\n");
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    if (!validate_uuid(uuid->valuestring)) {
        char esc[300];
        log_escape(uuid->valuestring, 64, esc, sizeof(esc));
        LOG_INFO("Error: Invalid UUID format: %s", esc);
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    strncpy(req->uuid, uuid->valuestring, sizeof(req->uuid) - 1);

    /* Parse from language */
    cJSON *from = cJSON_GetObjectItem(root, "from");
    if (!cJSON_IsString(from) || !from->valuestring) {
        fprintf(stderr, "Error: Missing or invalid 'from' field\n");
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    /* Check auto before general language normalization. */
    const char *from_code = normalize_source_language(from->valuestring);
    if (!from_code) {
        char esc[300];
        log_escape(from->valuestring, 64, esc, sizeof(esc));
        LOG_INFO("Error: Invalid 'from' language code or name: %s", esc);
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    strncpy(req->from_lang, from_code, sizeof(req->from_lang) - 1);

    /* Parse to language */
    cJSON *to = cJSON_GetObjectItem(root, "to");
    if (!cJSON_IsString(to) || !to->valuestring) {
        fprintf(stderr, "Error: Missing or invalid 'to' field\n");
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    /* Normalize language code/name to ISO 639-2 code */
    const char *to_code = normalize_language_code(to->valuestring);
    if (!to_code) {
        char esc[300];
        log_escape(to->valuestring, 64, esc, sizeof(esc));
        LOG_INFO("Error: Invalid 'to' language code or name: %s", esc);
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    strncpy(req->to_lang, to_code, sizeof(req->to_lang) - 1);

    /* Parse text */
    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text) || !text->valuestring) {
        fprintf(stderr, "Error: Missing or invalid 'text' field\n");
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    /* Validate text length */
    size_t text_len = strlen(text->valuestring);
    if (text_len < MIN_TEXT_LENGTH) {
        fprintf(stderr, "Error: Text is empty or too short\n");
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    if (text_len > MAX_TEXT_LENGTH) {
        fprintf(stderr, "Error: Text is too long (max %d characters)\n", MAX_TEXT_LENGTH);
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    req->text = strdup(text->valuestring);
    if (!req->text) {
        fprintf(stderr, "Error: Memory allocation failed for text\n");
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_Delete(root);
    return req;
}

/* Free translation request */
void free_translation_request(TranslationRequest *req) {
    if (!req) {
        return;
    }

    free(req->text);
    free(req);
}

/* Create translation response JSON */
char *create_translation_response(const TranslationRequest *req, const char *translated_text) {
    if (!req || !translated_text) {
        fprintf(stderr, "Error: NULL request or translated text\n");
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        fprintf(stderr, "Error: Failed to create JSON object\n");
        return NULL;
    }

    /* Add timestamp (preserved from request) */
    cJSON_AddStringToObject(root, "timestamp", req->timestamp);

    /* Add UUID (preserved from request) */
    cJSON_AddStringToObject(root, "uuid", req->uuid);

    /* Add translated text */
    cJSON_AddStringToObject(root, "translatedText", translated_text);

    /* Convert to string */
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        fprintf(stderr, "Error: Failed to serialize JSON\n");
        return NULL;
    }

    return json_str;
}

/* Create error response JSON */
char *create_error_response(const char *error_code, const char *error_message, const char *uuid) {
    if (!error_code || !error_message) {
        fprintf(stderr, "Error: NULL error code or message\n");
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        fprintf(stderr, "Error: Failed to create JSON object\n");
        return NULL;
    }

    /* Add error code */
    cJSON_AddStringToObject(root, "errorCode", error_code);

    /* Add error message */
    cJSON_AddStringToObject(root, "errorMessage", error_message);

    /* Add UUID if provided */
    if (uuid) {
        cJSON_AddStringToObject(root, "uuid", uuid);
    }

    /* Add current timestamp */
    char timestamp[64];
    if (get_current_timestamp(timestamp, sizeof(timestamp)) == 0) {
        cJSON_AddStringToObject(root, "timestamp", timestamp);
    }

    /* Convert to string */
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        fprintf(stderr, "Error: Failed to serialize JSON\n");
        return NULL;
    }

    return json_str;
}

/* Free JSON response string */
void free_json_response(char *json_str) {
    if (json_str) {
        free(json_str);
    }
}
