/**
 * HTTP server module for transbasket.
 * FastAPI equivalent using libmicrohttpd for HTTP handling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <microhttpd.h>
#include "http_server.h"
#include "json_handler.h"
#include "language_table.h"
#include "utils.h"
#include "trans_cache.h"

/* libmicrohttpd 0.9.74 renamed the 422 constant and deprecated the old name.
 * Define the new name ourselves on older releases so both versions build clean. */
#ifndef MHD_HTTP_UNPROCESSABLE_CONTENT
#define MHD_HTTP_UNPROCESSABLE_CONTENT 422
#endif

#define DEFAULT_MAX_WORKERS 30
#define TRUNCATE_DISPLAY_LENGTH 50
#define TRUNCATE_BUFFER_SIZE 100

/* Build an MHD response body; the HTTP status is applied by the caller
 * when it queues the response. */
static struct MHD_Response *create_json_response(const char *json_str) {
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

    struct MHD_Response *response = create_json_response(json_str);
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
            LOG_DEBUG("Cache periodically saved to disk");
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
static int handle_health_check(struct MHD_Connection *connection, TranslationServer *server) {
    /* Shallow health: status + active backend. Does NOT spawn codex. */
    char response_json[256];
    const char *model_type = (server && server->config && server->config->model_type_str)
                                 ? server->config->model_type_str : "normal";
    snprintf(response_json, sizeof(response_json),
             "{\"status\":\"healthy\",\"service\":\"transbasket\",\"version\":\"1.0.0\",\"model_type\":\"%s\"}",
             model_type);

    struct MHD_Response *response = create_json_response(response_json);
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
            *con_cls = NULL;
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
        return send_json_response(connection, error_json, MHD_HTTP_UNPROCESSABLE_CONTENT, false);
    }

    request_uuid = strdup(req->uuid);
    if (!request_uuid) {
        char *response = create_error_response("INTERNAL_ERROR", "Memory allocation failed", req->uuid);
        free_translation_request(req);
        return send_json_response(connection, response, MHD_HTTP_INTERNAL_SERVER_ERROR, false);
    }

    /* Strip ANSI escape codes and control characters from text */
    size_t text_len = strlen(req->text);
    char *cleaned_text = malloc(text_len + 1);
    if (!cleaned_text) {
        LOG_INFO("[%s] Memory allocation failed for ANSI stripping", request_uuid);
        free_translation_request(req);
        char *error_json = create_error_response("INTERNAL_ERROR",
                                                 "Memory allocation failed",
                                                 request_uuid);
        free(request_uuid);
        return send_json_response(connection, error_json, MHD_HTTP_INTERNAL_SERVER_ERROR, false);
    }

    if (strip_ansi_codes(req->text, cleaned_text, text_len + 1) != 0) {
        LOG_INFO("[%s] Failed to strip ANSI codes", request_uuid);
        free(cleaned_text);
        free_translation_request(req);
        char *error_json = create_error_response("INTERNAL_ERROR",
                                                 "Text processing failed",
                                                 request_uuid);
        free(request_uuid);
        return send_json_response(connection, error_json, MHD_HTTP_INTERNAL_SERVER_ERROR, false);
    }

    /* Strip control characters from text */
    char *control_filtered_text = malloc(strlen(cleaned_text) + 1);
    if (!control_filtered_text) {
        LOG_INFO("[%s] Memory allocation failed for control character stripping", request_uuid);
        free(cleaned_text);
        free_translation_request(req);
        char *error_json = create_error_response("INTERNAL_ERROR",
                                                 "Memory allocation failed",
                                                 request_uuid);
        free(request_uuid);
        return send_json_response(connection, error_json, MHD_HTTP_INTERNAL_SERVER_ERROR, false);
    }

    if (strip_control_characters(cleaned_text, control_filtered_text, strlen(cleaned_text) + 1) != 0) {
        LOG_INFO("[%s] Failed to strip control characters", request_uuid);
        free(control_filtered_text);
        free(cleaned_text);
        free_translation_request(req);
        char *error_json = create_error_response("INTERNAL_ERROR",
                                                 "Text processing failed",
                                                 request_uuid);
        free(request_uuid);
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

    /* Fast cache hits do not consume admission or waiter capacity. */
    CacheEntry *cached = server->cache
        ? trans_cache_lookup(server->cache, req->from_lang, req->to_lang, req->text) : NULL;
    if (cached && cached->count >= server->config->cache_threshold) goto cache_hit;
    trans_cache_entry_free(cached);
    cached = NULL;

    InflightHandle handle;
    InflightAdmission admission = inflight_acquire(server->inflight, server->cache,
        server->config->cache_threshold, req->from_lang, req->to_lang, req->text, &handle, &cached);
    if (admission == INFLIGHT_CACHE) goto cache_hit;
    if (admission != INFLIGHT_OWNER && admission != INFLIGHT_FOLLOWER) {
        trans_cache_entry_free(cached);
        int status = admission == INFLIGHT_ERROR ? 500 : 503;
        const char *message = admission == INFLIGHT_ERROR ? "Cannot allocate translation job" :
            admission == INFLIGHT_STOPPED ? "Server shutting down" : "Translation capacity reached";
        char *response = create_error_response(status == 500 ? "INTERNAL_ERROR" : "TRANSLATION_ERROR",
                                               message, request_uuid);
        free(request_uuid);
        free_translation_request(req);
        return send_json_response(connection, response, status, status == 503);
    }

    int64_t wait_started = translation_now_ms();
    LOG_INFO("[%s] Translation group %lu role=%s", request_uuid, inflight_id(&handle),
             handle.owner ? "owner" : "waiter");
    if (admission == INFLIGHT_OWNER) {
        const TranslationControl *control = inflight_control(&handle);
        TranslationError error = {0};
        char *translated = openai_translate(server->translator, req->from_lang, req->to_lang,
            req->text, request_uuid, req->timestamp, &error, control);
        int status = 200;
        if (!translated) {
            status = server->config->model_type == MODEL_TYPE_CODEX && error.status_code
                ? error.status_code : error.retryable ? 503 : 502;
        } else if (server->cache && translation_remaining_ms(control)) {
            int rc;
            if (!cached) {
                rc = trans_cache_add(server->cache, req->from_lang, req->to_lang, req->text, translated);
            } else if (strcmp(cached->translated_text, translated) == 0) {
                rc = trans_cache_update_count(server->cache, cached);
            } else {
                rc = trans_cache_update_translation(server->cache, cached, translated);
            }
            if (rc != 0) LOG_INFO("[%s] Cache write failed; sharing translation without persistence", request_uuid);
        }
        /* Publication owns the strings and happens even if this client went away. */
        trans_cache_entry_free(cached);
        cached = NULL;
        inflight_publish(server->inflight, &handle, translated, error.message, status, error.retryable);
    }

    InflightResult result = inflight_wait(server->inflight, &handle);
    LOG_INFO("[%s] Translation group %lu response status=%d elapsed_ms=%lld", request_uuid,
             inflight_id(&handle), result.status, (long long)(translation_now_ms() - wait_started));
    char *response = result.text ? create_translation_response(req, result.text) :
        create_error_response("TRANSLATION_ERROR", result.message, request_uuid);
    int status = result.status;
    bool retryable = result.retryable;
    inflight_release(server->inflight, &handle);
    free(request_uuid);
    free_translation_request(req);
    return send_json_response(connection, response, status, retryable);

cache_hit:
    trans_cache_update_count(server->cache, cached);
    response = create_translation_response(req, cached->translated_text);
    trans_cache_entry_free(cached);
    free(request_uuid);
    free_translation_request(req);
    return send_json_response(connection, response, MHD_HTTP_OK, false);
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
        return handle_health_check(connection, server);
    }

    /* Translation endpoint */
    if (strcmp(url, "/translate") == 0 && strcmp(method, "POST") == 0) {
        return handle_translate(connection, upload_data, upload_data_size, con_cls, server);
    }

    /* 404 Not Found */
    const char *not_found = "{\"error\":\"Not Found\"}";
    struct MHD_Response *response = create_json_response(not_found);

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

    /* Load language table */
    server->lang_table = NULL;
    if (config->language_table_file) {
        server->lang_table = language_table_load(config->language_table_file);
        if (server->lang_table) {
            LOG_INFO("Language table loaded: %zu entries, %zu code mappings from %s",
                    server->lang_table->count, server->lang_table->mapping_count,
                    config->language_table_file);
        } else {
            LOG_INFO("Warning: Failed to load language table from %s, continuing without it",
                    config->language_table_file);
        }
    }

    /* codex backend: ensure the per-request temp-cwd root exists/writable. */
    if (config->model_type == MODEL_TYPE_CODEX && config->codex_run_dir) {
        struct stat rst = {0};
        if (stat(config->codex_run_dir, &rst) == -1) {
            if (mkdir(config->codex_run_dir, 0700) == -1) {
                LOG_INFO("Warning: CODEX_RUN_DIR %s not present and could not be created (%s); "
                         "per-request cwd will fall back to none",
                        config->codex_run_dir, strerror(errno));
            } else {
                LOG_INFO("Created CODEX_RUN_DIR: %s", config->codex_run_dir);
            }
        } else if (access(config->codex_run_dir, W_OK) != 0) {
            LOG_INFO("Warning: CODEX_RUN_DIR %s is not writable; per-request cwd will fall back to none",
                    config->codex_run_dir);
        }
    }

    /* Initialize translator with language table.
     * codex concurrency = CODEX_MAX_CONCURRENCY or MAX_WORKERS. */
    int codex_conc = config->codex_max_concurrency > 0
                         ? config->codex_max_concurrency : server->max_workers;
    server->translator = openai_translator_init(config, server->lang_table, 3, 60, codex_conc);
    if (!server->translator) {
        LOG_INFO("Error: Failed to initialize translator");
        if (server->lang_table) {
            language_table_free(server->lang_table);
        }
        free(server);
        return NULL;
    }

    server->inflight = inflight_create(
        config->inflight_max_keys > 0 ? config->inflight_max_keys : server->max_workers,
        config->inflight_max_waiters, config->inflight_max_waiters_per_key,
        config->translation_total_timeout, config->model_type == MODEL_TYPE_CODEX ? 504 : 503);
    if (!server->inflight) {
        LOG_INFO("Error: Could not initialize translation groups");
        openai_translator_free(server->translator);
        language_table_free(server->lang_table);
        free(server);
        return NULL;
    }
    LOG_INFO("Translation groups initialized: keys=%d, waiters=%d, per_key=%d, total_timeout=%ds",
             config->inflight_max_keys > 0 ? config->inflight_max_keys : server->max_workers,
             config->inflight_max_waiters, config->inflight_max_waiters_per_key,
             config->translation_total_timeout);
    atomic_init(&server->cache_bg_running, false);

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

    inflight_shutdown(server->inflight);
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

    inflight_destroy(server->inflight);
    server->inflight = NULL;

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

    if (server->lang_table) {
        language_table_free(server->lang_table);
    }

    free(server);
}
