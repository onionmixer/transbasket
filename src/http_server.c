/**
 * HTTP server module for transbasket.
 * FastAPI equivalent using libmicrohttpd for HTTP handling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <microhttpd.h>
#include "http_server.h"
#include "json_handler.h"
#include "utils.h"
#include "trans_cache.h"

#define DEFAULT_MAX_WORKERS 30
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

/* Cache background thread - handles periodic save and cleanup */
static void *cache_background_thread(void *arg) {
    TranslationServer *server = (TranslationServer *)arg;

    /* Save interval: 5 seconds */
    const int save_interval = 5;

    /* Cleanup interval and settings */
    int cleanup_enabled = server->config->cache_cleanup_enabled;
    int cleanup_days = server->config->cache_cleanup_days;
    int cleanup_check_interval = 0;

    if (cleanup_enabled) {
        /* Cleanup interval: 1/10 of cleanup days or minimum 1 hour */
        cleanup_check_interval = (cleanup_days * 24 * 60 * 60) / 10;
        if (cleanup_check_interval < 3600) {
            cleanup_check_interval = 3600;  /* Minimum 1 hour */
        }
        LOG_DEBUG("Cache background thread started (save every %d seconds, cleanup check every %d seconds, cleanup after %d days)",
                save_interval, cleanup_check_interval, cleanup_days);
    } else {
        LOG_DEBUG("Cache background thread started (save every %d seconds, cleanup disabled)",
                save_interval);
    }

    int elapsed = 0;

    while (server->cache_bg_running) {
        sleep(save_interval);
        elapsed += save_interval;

        if (!server->cache_bg_running) break;

        /* Periodic save */
        if (trans_cache_save(server->cache) == 0) {
            LOG_DEBUG("Cache periodically saved to disk (%d entries)", (int)server->cache->size);
        }

        /* Cleanup check (if enabled) */
        if (cleanup_enabled && elapsed >= cleanup_check_interval) {
            elapsed = 0;

            int removed = trans_cache_cleanup(server->cache, cleanup_days);

            if (removed > 0) {
                LOG_INFO("Cache cleanup: removed %d expired entries", removed);
            }
        }
    }

    LOG_DEBUG("Cache background thread stopped");
    return NULL;
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

    /* Strip ANSI escape codes and control characters from text */
    size_t text_len = strlen(req->text);
    char *cleaned_text = malloc(text_len + 1);
    if (!cleaned_text) {
        LOG_INFO("[%s] Memory allocation failed for ANSI stripping", request_uuid);
        free(request_uuid);
        free_translation_request(req);
        char *error_json = create_error_response("INTERNAL_ERROR",
                                                 "Memory allocation failed",
                                                 request_uuid);
        return send_json_response(connection, error_json, MHD_HTTP_INTERNAL_SERVER_ERROR, false);
    }

    if (strip_ansi_codes(req->text, cleaned_text, text_len + 1) != 0) {
        LOG_INFO("[%s] Failed to strip ANSI codes", request_uuid);
        free(cleaned_text);
        free(request_uuid);
        free_translation_request(req);
        char *error_json = create_error_response("INTERNAL_ERROR",
                                                 "Text processing failed",
                                                 request_uuid);
        return send_json_response(connection, error_json, MHD_HTTP_INTERNAL_SERVER_ERROR, false);
    }

    /* Strip control characters from text */
    char *control_filtered_text = malloc(strlen(cleaned_text) + 1);
    if (!control_filtered_text) {
        LOG_INFO("[%s] Memory allocation failed for control character stripping", request_uuid);
        free(cleaned_text);
        free(request_uuid);
        free_translation_request(req);
        char *error_json = create_error_response("INTERNAL_ERROR",
                                                 "Memory allocation failed",
                                                 request_uuid);
        return send_json_response(connection, error_json, MHD_HTTP_INTERNAL_SERVER_ERROR, false);
    }

    if (strip_control_characters(cleaned_text, control_filtered_text, strlen(cleaned_text) + 1) != 0) {
        LOG_INFO("[%s] Failed to strip control characters", request_uuid);
        free(control_filtered_text);
        free(cleaned_text);
        free(request_uuid);
        free_translation_request(req);
        char *error_json = create_error_response("INTERNAL_ERROR",
                                                 "Text processing failed",
                                                 request_uuid);
        return send_json_response(connection, error_json, MHD_HTTP_INTERNAL_SERVER_ERROR, false);
    }

    /* Replace original text with fully cleaned text */
    free(req->text);
    free(cleaned_text);
    req->text = control_filtered_text;

    char truncated_text[TRUNCATE_BUFFER_SIZE];
    truncate_text(req->text, truncated_text, TRUNCATE_DISPLAY_LENGTH, "...");
    LOG_INFO("[%s] Translation request received: %s -> %s, text: %s",
            request_uuid, req->from_lang, req->to_lang, truncated_text);

    /* Check cache first if enabled */
    CacheEntry *cached = NULL;
    if (server->cache) {
        cached = trans_cache_lookup(server->cache, req->from_lang, req->to_lang, req->text);

        if (cached && cached->count >= server->config->cache_threshold) {
            /* Cache hit - use cached translation */
            LOG_DEBUG("[%s] Cache hit (count: %d >= threshold: %d)",
                    request_uuid, cached->count, server->config->cache_threshold);

            /* Increment count */
            trans_cache_update_count(server->cache, cached);

            /* Create response with cached translation */
            char *response_json = create_translation_response(req, cached->translated_text);

            char truncated_result[TRUNCATE_BUFFER_SIZE];
            truncate_text(cached->translated_text, truncated_result, TRUNCATE_DISPLAY_LENGTH, "...");
            LOG_INFO("[%s] Translation from cache, result: %s", request_uuid, truncated_result);

            free(request_uuid);
            free_translation_request(req);

            return send_json_response(connection, response_json, MHD_HTTP_OK, false);
        }

        if (cached) {
            LOG_DEBUG("[%s] Cache found but count insufficient (%d < %d), requesting API",
                    request_uuid, cached->count, server->config->cache_threshold);
        }
    }

    /* Perform translation via OpenAI API */
    TranslationError trans_error = {0};
    char *translated_text = openai_translate(
        server->translator,
        req->from_lang,
        req->to_lang,
        req->text,
        request_uuid,
        req->timestamp,
        &trans_error
    );

    if (!translated_text) {
        LOG_INFO("[%s] Translation error: %s", request_uuid,
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

    /* Update cache with translation result */
    if (server->cache) {
        if (cached) {
            /* Existing cache entry - check if translation matches */
            if (strcmp(cached->translated_text, translated_text) == 0) {
                /* Same translation - increment count */
                trans_cache_update_count(server->cache, cached);
                LOG_DEBUG("[%s] Cache updated (same translation, count: %d)",
                        request_uuid, cached->count + 1);
            } else {
                /* Different translation - update translation and reset count */
                trans_cache_update_translation(server->cache, cached, translated_text);
                LOG_DEBUG("[%s] Cache updated (different translation, count reset to 1)",
                        request_uuid);
            }
        } else {
            /* New cache entry */
            if (trans_cache_add(server->cache, req->from_lang, req->to_lang,
                               req->text, translated_text) == 0) {
                LOG_DEBUG("[%s] Added to cache (count: 1)", request_uuid);
            }
        }
    }

    /* Create success response */
    char *response_json = create_translation_response(req, translated_text);

    char truncated_result[TRUNCATE_BUFFER_SIZE];
    truncate_text(translated_text, truncated_result, TRUNCATE_DISPLAY_LENGTH, "...");
    LOG_INFO("[%s] Translation completed, result: %s", request_uuid, truncated_result);

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
        LOG_INFO("Error: NULL config");
        return NULL;
    }

    TranslationServer *server = calloc(1, sizeof(TranslationServer));
    if (!server) {
        LOG_INFO("Error: Memory allocation failed");
        return NULL;
    }

    server->config = config;
    server->max_workers = max_workers > 0 ? max_workers : DEFAULT_MAX_WORKERS;

    /* Initialize translator */
    server->translator = openai_translator_init(config, 3, 60);
    if (!server->translator) {
        LOG_INFO("Error: Failed to initialize translator");
        free(server);
        return NULL;
    }

    /* Initialize cache */
    server->cache = NULL;
    server->cache_bg_running = false;

    /* Determine cache path based on backend type */
    const char *cache_path = NULL;
    switch (config->cache_type) {
        case CACHE_BACKEND_SQLITE:
            cache_path = config->cache_sqlite_path;
            break;
        case CACHE_BACKEND_TEXT:
        default:
            cache_path = config->cache_file;
            break;
    }

    if (cache_path) {
        server->cache = trans_cache_init_with_backend(config->cache_type, cache_path, NULL);
        if (!server->cache) {
            LOG_INFO("Warning: Failed to initialize cache, continuing without cache");
        } else {
            LOG_INFO("Translation cache initialized: %s backend at %s (threshold: %d)",
                    config->cache_type_str, cache_path, config->cache_threshold);

            /* Always start background thread for periodic cache saving */
            server->cache_bg_running = true;
            if (pthread_create(&server->cache_bg_thread, NULL,
                             cache_background_thread, server) != 0) {
                LOG_INFO("Warning: Failed to start cache background thread");
                server->cache_bg_running = false;
            } else {
                LOG_DEBUG("Cache background thread started (cleanup %s)",
                        config->cache_cleanup_enabled ? "enabled" : "disabled");
            }
        }
    }

    LOG_INFO("Translation server initialized with %d workers", server->max_workers);

    return server;
}

/* Start translation server */
int translation_server_start(TranslationServer *server) {
    if (!server) {
        LOG_INFO("Error: NULL server");
        return -1;
    }

    LOG_INFO("Starting HTTP server on %s:%d...",
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
        LOG_INFO("Error: Failed to start HTTP server");
        return -1;
    }

    LOG_INFO("HTTP server started successfully on %s:%d",
            server->config->listen, server->config->port);

    return 0;
}

/* Stop translation server */
void translation_server_stop(TranslationServer *server) {
    if (!server) {
        return;
    }

    LOG_INFO("Stopping HTTP server...");

    if (server->daemon) {
        MHD_stop_daemon(server->daemon);
        server->daemon = NULL;
    }

    LOG_INFO("HTTP server stopped");
}

/* Free translation server */
void translation_server_free(TranslationServer *server) {
    if (!server) {
        return;
    }

    translation_server_stop(server);

    /* Stop cache background thread if running */
    if (server->cache_bg_running) {
        LOG_DEBUG("Stopping cache background thread...");
        server->cache_bg_running = false;
        pthread_join(server->cache_bg_thread, NULL);
        LOG_DEBUG("Cache background thread stopped");
    }

    /* Save and free cache */
    if (server->cache) {
        LOG_INFO("Saving translation cache...");
        trans_cache_save(server->cache);
        trans_cache_free(server->cache);
        LOG_INFO("Translation cache saved and freed");
    }

    if (server->translator) {
        openai_translator_free(server->translator);
    }

    free(server);
}
