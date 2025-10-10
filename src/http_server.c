/**
 * HTTP server module for transbasket.
 * FastAPI equivalent using libmicrohttpd for HTTP handling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <microhttpd.h>
#include "http_server.h"
#include "json_handler.h"
#include "utils.h"

#define DEFAULT_MAX_WORKERS 10
#define TRUNCATE_DISPLAY_LENGTH 50
#define TRUNCATE_BUFFER_SIZE 100

/* Response helper function */
static struct MHD_Response *create_json_response(const char *json_str, int status_code) {
    struct MHD_Response *response = MHD_create_response_from_buffer(
        strlen(json_str), (void *)json_str, MHD_RESPMEM_MUST_COPY);

    if (response) {
        MHD_add_response_header(response, "Content-Type", "application/json");
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    }

    return response;
}

/* Helper function to send JSON response and cleanup */
static int send_json_response(struct MHD_Connection *connection,
                              char *json_str, int status_code, bool add_retry_header) {
    if (!json_str) {
        return MHD_NO;
    }

    struct MHD_Response *response = create_json_response(json_str, status_code);
    free_json_response(json_str);

    if (!response) {
        return MHD_NO;
    }

    if (add_retry_header) {
        MHD_add_response_header(response, "Retry-After", "5");
    }

    int ret = MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);

    return ret;
}

/* Health check endpoint handler */
static int handle_health_check(struct MHD_Connection *connection) {
    const char *response_json = "{\"status\":\"healthy\",\"service\":\"transbasket\",\"version\":\"1.0.0\"}";

    struct MHD_Response *response = create_json_response(response_json, MHD_HTTP_OK);
    if (!response) {
        return MHD_NO;
    }

    int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);

    return ret;
}

/* Translation endpoint handler */
static int handle_translate(struct MHD_Connection *connection, const char *upload_data,
                           size_t *upload_data_size, void **con_cls,
                           TranslationServer *server) {
    /* First call - setup connection */
    if (*con_cls == NULL) {
        char *buffer = malloc(1);
        if (!buffer) {
            return MHD_NO;
        }
        buffer[0] = '\0';
        *con_cls = buffer;
        return MHD_YES;
    }

    /* Accumulate POST data */
    if (*upload_data_size != 0) {
        char *buffer = *con_cls;
        size_t current_size = strlen(buffer);
        char *new_buffer = realloc(buffer, current_size + *upload_data_size + 1);

        if (!new_buffer) {
            free(buffer);
            return MHD_NO;
        }

        memcpy(new_buffer + current_size, upload_data, *upload_data_size);
        new_buffer[current_size + *upload_data_size] = '\0';

        *con_cls = new_buffer;
        *upload_data_size = 0;

        return MHD_YES;
    }

    /* Process request */
    char *request_data = *con_cls;
    char *request_uuid = NULL;

    /* Parse translation request */
    TranslationRequest *req = parse_translation_request(request_data);
    free(request_data);
    *con_cls = NULL;

    if (!req) {
        char *error_json = create_error_response("VALIDATION_ERROR",
                                                 "Request validation failed",
                                                 NULL);
        return send_json_response(connection, error_json, MHD_HTTP_UNPROCESSABLE_ENTITY, false);
    }

    request_uuid = strdup(req->uuid);

    char truncated_text[TRUNCATE_BUFFER_SIZE];
    truncate_text(req->text, truncated_text, TRUNCATE_DISPLAY_LENGTH, "...");
    fprintf(stderr, "[%s] Translation request received: %s -> %s, text: %s\n",
            request_uuid, req->from_lang, req->to_lang, truncated_text);

    /* Perform translation */
    TranslationError trans_error = {0};
    char *translated_text = openai_translate(
        server->translator,
        req->from_lang,
        req->to_lang,
        req->text,
        request_uuid,
        &trans_error
    );

    if (!translated_text) {
        fprintf(stderr, "[%s] Translation error: %s\n", request_uuid,
                trans_error.message ? trans_error.message : "Unknown error");

        int status_code = trans_error.retryable ? MHD_HTTP_SERVICE_UNAVAILABLE : MHD_HTTP_BAD_GATEWAY;

        char *error_json = create_error_response("TRANSLATION_ERROR",
                                                 trans_error.message ? trans_error.message : "Translation failed",
                                                 request_uuid);

        free(trans_error.message);
        free(request_uuid);
        free_translation_request(req);

        return send_json_response(connection, error_json, status_code, trans_error.retryable);
    }

    /* Create success response */
    char *response_json = create_translation_response(req, translated_text);

    char truncated_result[TRUNCATE_BUFFER_SIZE];
    truncate_text(translated_text, truncated_result, TRUNCATE_DISPLAY_LENGTH, "...");
    fprintf(stderr, "[%s] Translation completed, result: %s\n", request_uuid, truncated_result);

    free_translated_text(translated_text);
    free(request_uuid);
    free_translation_request(req);

    return send_json_response(connection, response_json, MHD_HTTP_OK, false);
}

/* Main request handler */
static enum MHD_Result request_handler(void *cls, struct MHD_Connection *connection,
                                      const char *url, const char *method,
                                      const char *version, const char *upload_data,
                                      size_t *upload_data_size, void **con_cls) {
    (void)version;  /* Unused */

    TranslationServer *server = (TranslationServer *)cls;

    /* Health check endpoint */
    if (strcmp(url, "/health") == 0 && strcmp(method, "GET") == 0) {
        return handle_health_check(connection);
    }

    /* Translation endpoint */
    if (strcmp(url, "/translate") == 0 && strcmp(method, "POST") == 0) {
        return handle_translate(connection, upload_data, upload_data_size, con_cls, server);
    }

    /* 404 Not Found */
    const char *not_found = "{\"error\":\"Not Found\"}";
    struct MHD_Response *response = create_json_response(not_found, MHD_HTTP_NOT_FOUND);

    if (!response) {
        return MHD_NO;
    }

    int ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
    MHD_destroy_response(response);

    return ret;
}

/* Request completed callback */
static void request_completed(void *cls, struct MHD_Connection *connection,
                             void **con_cls, enum MHD_RequestTerminationCode toe) {
    (void)cls;
    (void)connection;
    (void)toe;

    if (*con_cls != NULL) {
        free(*con_cls);
        *con_cls = NULL;
    }
}

/* Initialize translation server */
TranslationServer *translation_server_init(Config *config, int max_workers) {
    if (!config) {
        fprintf(stderr, "Error: NULL config\n");
        return NULL;
    }

    TranslationServer *server = calloc(1, sizeof(TranslationServer));
    if (!server) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        return NULL;
    }

    server->config = config;
    server->max_workers = max_workers > 0 ? max_workers : DEFAULT_MAX_WORKERS;

    /* Initialize translator */
    server->translator = openai_translator_init(config, 3, 60);
    if (!server->translator) {
        fprintf(stderr, "Error: Failed to initialize translator\n");
        free(server);
        return NULL;
    }

    fprintf(stderr, "Translation server initialized with %d workers\n", server->max_workers);

    return server;
}

/* Start translation server */
int translation_server_start(TranslationServer *server) {
    if (!server) {
        fprintf(stderr, "Error: NULL server\n");
        return -1;
    }

    fprintf(stderr, "Starting HTTP server on %s:%d...\n",
            server->config->listen, server->config->port);

    server->daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION,
        server->config->port,
        NULL, NULL,
        &request_handler, server,
        MHD_OPTION_NOTIFY_COMPLETED, &request_completed, NULL,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)120,
        MHD_OPTION_END
    );

    if (!server->daemon) {
        fprintf(stderr, "Error: Failed to start HTTP server\n");
        return -1;
    }

    fprintf(stderr, "HTTP server started successfully on %s:%d\n",
            server->config->listen, server->config->port);

    return 0;
}

/* Stop translation server */
void translation_server_stop(TranslationServer *server) {
    if (!server) {
        return;
    }

    fprintf(stderr, "Stopping HTTP server...\n");

    if (server->daemon) {
        MHD_stop_daemon(server->daemon);
        server->daemon = NULL;
    }

    fprintf(stderr, "HTTP server stopped\n");
}

/* Free translation server */
void translation_server_free(TranslationServer *server) {
    if (!server) {
        return;
    }

    translation_server_stop(server);

    if (server->translator) {
        openai_translator_free(server->translator);
    }

    free(server);
}
