#ifndef JSON_HANDLER_H
#define JSON_HANDLER_H

#include <cjson/cJSON.h>

/* Translation request structure */
typedef struct {
    char timestamp[64];
    char uuid[37];
    char from_lang[4];
    char to_lang[4];
    char *text;
} TranslationRequest;

/* Parse translation request from JSON string */
TranslationRequest *parse_translation_request(const char *json_str);

/* Free translation request */
void free_translation_request(TranslationRequest *req);

/* Create translation response JSON */
char *create_translation_response(const TranslationRequest *req, const char *translated_text);

/* Create error response JSON */
char *create_error_response(const char *error_code, const char *error_message, const char *uuid);

/* Free JSON response string */
void free_json_response(char *json_str);

#endif /* JSON_HANDLER_H */
