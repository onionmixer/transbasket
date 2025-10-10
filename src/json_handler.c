/**
 * JSON handler module for transbasket.
 * Handles JSON request/response parsing and generation using cJSON.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include "json_handler.h"
#include "utils.h"

#define MAX_TEXT_LENGTH 10000
#define MIN_TEXT_LENGTH 1

/* Parse translation request from JSON string */
TranslationRequest *parse_translation_request(const char *json_str) {
    if (!json_str) {
        fprintf(stderr, "Error: NULL JSON string\n");
        return NULL;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        fprintf(stderr, "Error: Failed to parse JSON: %s\n", cJSON_GetErrorPtr());
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
        fprintf(stderr, "Error: Invalid timestamp format: %s\n", timestamp->valuestring);
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

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
        fprintf(stderr, "Error: Invalid UUID format: %s\n", uuid->valuestring);
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

    if (!validate_language_code(from->valuestring)) {
        fprintf(stderr, "Error: Invalid 'from' language code: %s\n", from->valuestring);
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    strncpy(req->from_lang, from->valuestring, sizeof(req->from_lang) - 1);

    /* Parse to language */
    cJSON *to = cJSON_GetObjectItem(root, "to");
    if (!cJSON_IsString(to) || !to->valuestring) {
        fprintf(stderr, "Error: Missing or invalid 'to' field\n");
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    if (!validate_language_code(to->valuestring)) {
        fprintf(stderr, "Error: Invalid 'to' language code: %s\n", to->valuestring);
        free(req);
        cJSON_Delete(root);
        return NULL;
    }

    strncpy(req->to_lang, to->valuestring, sizeof(req->to_lang) - 1);

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
