/**
 * Text (JSONL) backend implementation for translation cache.
 * This backend stores cache entries in a JSONL file format.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cjson/cJSON.h>
#include "cache_backend_text.h"
#include "trans_cache.h"
#include "utils.h"

#define INITIAL_CAPACITY 100
#define GROWTH_FACTOR 2

/* Forward declarations of backend operations */
static CacheEntry* text_backend_lookup(void *ctx, const char *from_lang,
                                       const char *to_lang, const char *text);
static int text_backend_add(void *ctx, const char *from_lang, const char *to_lang,
                            const char *source_text, const char *translated_text);
static int text_backend_update_count(void *ctx, CacheEntry *entry);
static int text_backend_update_translation(void *ctx, CacheEntry *entry,
                                           const char *new_translation);
static int text_backend_save(void *ctx);
static int text_backend_cleanup(void *ctx, int days_threshold);
static void text_backend_stats(void *ctx, size_t *total_entries,
                               size_t *active_entries, size_t *expired_entries,
                               int cache_threshold, int days_threshold);
static void text_backend_free(void *ctx);

/* Load cache entries from JSONL file */
static int load_cache_from_file(TextBackendContext *ctx, const char *file_path) {
    FILE *fp = fopen(file_path, "r");
    if (!fp) {
        /* File doesn't exist yet - this is OK */
        LOG_DEBUG("Cache file not found, will create new: %s\n", file_path);
        return 0;
    }

    char *line = NULL;
    size_t line_len = 0;
    ssize_t read;
    int loaded_count = 0;

    while ((read = getline(&line, &line_len, fp)) != -1) {
        /* Parse JSON line */
        cJSON *json = cJSON_Parse(line);
        if (!json) {
            LOG_DEBUG("Warning: Failed to parse cache line, skipping\n");
            continue;
        }

        /* Extract fields */
        cJSON *id_json = cJSON_GetObjectItem(json, "id");
        cJSON *hash_json = cJSON_GetObjectItem(json, "hash");
        cJSON *from_json = cJSON_GetObjectItem(json, "from");
        cJSON *to_json = cJSON_GetObjectItem(json, "to");
        cJSON *source_json = cJSON_GetObjectItem(json, "source");
        cJSON *target_json = cJSON_GetObjectItem(json, "target");
        cJSON *count_json = cJSON_GetObjectItem(json, "count");
        cJSON *last_used_json = cJSON_GetObjectItem(json, "last_used");
        cJSON *created_at_json = cJSON_GetObjectItem(json, "created_at");

        /* Validate required fields */
        if (!cJSON_IsNumber(id_json) || !cJSON_IsString(hash_json) ||
            !cJSON_IsString(from_json) || !cJSON_IsString(to_json) ||
            !cJSON_IsString(source_json) || !cJSON_IsString(target_json) ||
            !cJSON_IsNumber(count_json) || !cJSON_IsNumber(last_used_json) ||
            !cJSON_IsNumber(created_at_json)) {
            LOG_DEBUG("Warning: Invalid cache entry format, skipping\n");
            cJSON_Delete(json);
            continue;
        }

        /* Allocate cache entry */
        CacheEntry *entry = calloc(1, sizeof(CacheEntry));
        if (!entry) {
            LOG_DEBUG("Error: Memory allocation failed\n");
            cJSON_Delete(json);
            break;
        }

        /* Copy data */
        entry->id = id_json->valueint;
        strncpy(entry->hash, hash_json->valuestring, sizeof(entry->hash) - 1);
        strncpy(entry->from_lang, from_json->valuestring, sizeof(entry->from_lang) - 1);
        strncpy(entry->to_lang, to_json->valuestring, sizeof(entry->to_lang) - 1);
        entry->source_text = strdup(source_json->valuestring);
        entry->translated_text = strdup(target_json->valuestring);
        entry->count = count_json->valueint;
        entry->last_used = (time_t)last_used_json->valuedouble;
        entry->created_at = (time_t)created_at_json->valuedouble;

        if (!entry->source_text || !entry->translated_text) {
            LOG_DEBUG("Error: Memory allocation failed\n");
            free(entry->source_text);
            free(entry->translated_text);
            free(entry);
            cJSON_Delete(json);
            break;
        }

        /* Expand capacity if needed */
        if (ctx->size >= ctx->capacity) {
            size_t new_capacity = ctx->capacity * GROWTH_FACTOR;
            CacheEntry **new_entries = realloc(ctx->entries,
                                              new_capacity * sizeof(CacheEntry *));
            if (!new_entries) {
                LOG_DEBUG("Error: Memory reallocation failed\n");
                free(entry->source_text);
                free(entry->translated_text);
                free(entry);
                cJSON_Delete(json);
                break;
            }
            ctx->entries = new_entries;
            ctx->capacity = new_capacity;
        }

        /* Add to cache */
        ctx->entries[ctx->size++] = entry;
        loaded_count++;

        /* Update next_id */
        if (entry->id >= ctx->next_id) {
            ctx->next_id = entry->id + 1;
        }

        cJSON_Delete(json);
    }

    free(line);
    fclose(fp);

    LOG_INFO("Loaded %d cache entries from %s\n", loaded_count, file_path);
    return loaded_count;
}

/* Initialize text backend */
TransCache *text_backend_init(const char *file_path) {
    if (!file_path) {
        LOG_DEBUG("Error: NULL file path\n");
        return NULL;
    }

    /* Allocate TransCache */
    TransCache *cache = calloc(1, sizeof(TransCache));
    if (!cache) {
        LOG_DEBUG("Error: Memory allocation failed\n");
        return NULL;
    }

    /* Allocate TextBackendContext */
    TextBackendContext *ctx = calloc(1, sizeof(TextBackendContext));
    if (!ctx) {
        LOG_DEBUG("Error: Memory allocation failed\n");
        free(cache);
        return NULL;
    }

    /* Allocate initial capacity */
    ctx->entries = malloc(INITIAL_CAPACITY * sizeof(CacheEntry *));
    if (!ctx->entries) {
        LOG_DEBUG("Error: Memory allocation failed\n");
        free(ctx);
        free(cache);
        return NULL;
    }

    ctx->size = 0;
    ctx->capacity = INITIAL_CAPACITY;
    ctx->file_path = strdup(file_path);
    ctx->next_id = 1;

    if (!ctx->file_path) {
        LOG_DEBUG("Error: Memory allocation failed\n");
        free(ctx->entries);
        free(ctx);
        free(cache);
        return NULL;
    }

    /* Initialize read-write lock */
    if (pthread_rwlock_init(&cache->lock, NULL) != 0) {
        LOG_DEBUG("Error: Failed to initialize rwlock\n");
        free(ctx->file_path);
        free(ctx->entries);
        free(ctx);
        free(cache);
        return NULL;
    }

    /* Set up TransCache */
    cache->type = CACHE_BACKEND_TEXT;
    cache->backend_ctx = ctx;
    cache->ops = text_backend_get_ops();

    /* Load existing cache from file */
    load_cache_from_file(ctx, file_path);

    return cache;
}

/* Lookup cache entry */
static CacheEntry* text_backend_lookup(void *backend_ctx,
                                       const char *from_lang,
                                       const char *to_lang,
                                       const char *text) {
    if (!backend_ctx || !from_lang || !to_lang || !text) {
        return NULL;
    }

    TextBackendContext *ctx = (TextBackendContext*)backend_ctx;

    /* Calculate hash */
    char hash[65];
    trans_cache_calculate_hash(from_lang, to_lang, text, hash);

    /* Search for entry (no lock needed - caller handles locking) */
    CacheEntry *found = NULL;
    for (size_t i = 0; i < ctx->size; i++) {
        if (strcmp(ctx->entries[i]->hash, hash) == 0) {
            found = ctx->entries[i];
            break;
        }
    }

    /* Update last_used timestamp if found */
    if (found) {
        found->last_used = time(NULL);
    }

    return found;
}

/* Add new cache entry */
static int text_backend_add(void *backend_ctx,
                           const char *from_lang,
                           const char *to_lang,
                           const char *source_text,
                           const char *translated_text) {
    if (!backend_ctx || !from_lang || !to_lang || !source_text || !translated_text) {
        return -1;
    }

    TextBackendContext *ctx = (TextBackendContext*)backend_ctx;

    /* Allocate new entry */
    CacheEntry *entry = calloc(1, sizeof(CacheEntry));
    if (!entry) {
        LOG_DEBUG("Error: Memory allocation failed\n");
        return -1;
    }

    /* Calculate hash */
    trans_cache_calculate_hash(from_lang, to_lang, source_text, entry->hash);

    /* Fill entry data */
    entry->id = ctx->next_id++;
    strncpy(entry->from_lang, from_lang, sizeof(entry->from_lang) - 1);
    strncpy(entry->to_lang, to_lang, sizeof(entry->to_lang) - 1);
    entry->source_text = strdup(source_text);
    entry->translated_text = strdup(translated_text);
    entry->count = 1;
    entry->created_at = time(NULL);
    entry->last_used = time(NULL);

    if (!entry->source_text || !entry->translated_text) {
        LOG_DEBUG("Error: Memory allocation failed\n");
        free(entry->source_text);
        free(entry->translated_text);
        free(entry);
        return -1;
    }

    /* Expand capacity if needed */
    if (ctx->size >= ctx->capacity) {
        size_t new_capacity = ctx->capacity * GROWTH_FACTOR;
        CacheEntry **new_entries = realloc(ctx->entries,
                                          new_capacity * sizeof(CacheEntry *));
        if (!new_entries) {
            LOG_DEBUG("Error: Memory reallocation failed\n");
            free(entry->source_text);
            free(entry->translated_text);
            free(entry);
            return -1;
        }
        ctx->entries = new_entries;
        ctx->capacity = new_capacity;
    }

    /* Add to cache */
    ctx->entries[ctx->size++] = entry;

    return 0;
}

/* Update cache entry count */
static int text_backend_update_count(void *backend_ctx, CacheEntry *entry) {
    if (!backend_ctx || !entry) {
        return -1;
    }

    entry->count++;
    entry->last_used = time(NULL);

    return 0;
}

/* Update cache entry translation */
static int text_backend_update_translation(void *backend_ctx,
                                           CacheEntry *entry,
                                           const char *new_translation) {
    if (!backend_ctx || !entry || !new_translation) {
        return -1;
    }

    /* Free old translation */
    free(entry->translated_text);

    /* Set new translation */
    entry->translated_text = strdup(new_translation);
    if (!entry->translated_text) {
        LOG_DEBUG("Error: Memory allocation failed\n");
        return -1;
    }

    /* Reset count to 1 */
    entry->count = 1;
    entry->last_used = time(NULL);

    return 0;
}

/* Save cache to file */
static int text_backend_save(void *backend_ctx) {
    if (!backend_ctx) {
        return -1;
    }

    TextBackendContext *ctx = (TextBackendContext*)backend_ctx;

    if (!ctx->file_path) {
        return -1;
    }

    FILE *fp = fopen(ctx->file_path, "w");
    if (!fp) {
        LOG_DEBUG("Error: Failed to open cache file for writing: %s\n",
                ctx->file_path);
        return -1;
    }

    for (size_t i = 0; i < ctx->size; i++) {
        CacheEntry *entry = ctx->entries[i];

        /* Create JSON object */
        cJSON *json = cJSON_CreateObject();
        if (!json) {
            LOG_DEBUG("Error: Failed to create JSON object\n");
            continue;
        }

        cJSON_AddNumberToObject(json, "id", entry->id);
        cJSON_AddStringToObject(json, "hash", entry->hash);
        cJSON_AddStringToObject(json, "from", entry->from_lang);
        cJSON_AddStringToObject(json, "to", entry->to_lang);
        cJSON_AddStringToObject(json, "source", entry->source_text);
        cJSON_AddStringToObject(json, "target", entry->translated_text);
        cJSON_AddNumberToObject(json, "count", entry->count);
        cJSON_AddNumberToObject(json, "last_used", (double)entry->last_used);
        cJSON_AddNumberToObject(json, "created_at", (double)entry->created_at);

        /* Write to file */
        char *json_str = cJSON_PrintUnformatted(json);
        if (json_str) {
            fprintf(fp, "%s\n", json_str);
            free(json_str);
        }

        cJSON_Delete(json);
    }

    fclose(fp);

    return 0;
}

/* Cleanup old cache entries */
static int text_backend_cleanup(void *backend_ctx, int days_threshold) {
    if (!backend_ctx || days_threshold <= 0) {
        return 0;
    }

    TextBackendContext *ctx = (TextBackendContext*)backend_ctx;

    time_t now = time(NULL);
    time_t threshold_time = now - (days_threshold * 24 * 60 * 60);

    int removed_count = 0;
    size_t write_idx = 0;

    /* Iterate and keep only valid entries */
    for (size_t i = 0; i < ctx->size; i++) {
        CacheEntry *entry = ctx->entries[i];

        if (entry->last_used < threshold_time) {
            /* Expired entry - free memory */
            free(entry->source_text);
            free(entry->translated_text);
            free(entry);
            removed_count++;
        } else {
            /* Valid entry - keep it */
            ctx->entries[write_idx++] = entry;
        }
    }

    ctx->size = write_idx;

    return removed_count;
}

/* Get cache statistics */
static void text_backend_stats(void *backend_ctx,
                               size_t *total_entries,
                               size_t *active_entries,
                               size_t *expired_entries,
                               int cache_threshold,
                               int days_threshold) {
    if (!backend_ctx) {
        return;
    }

    TextBackendContext *ctx = (TextBackendContext*)backend_ctx;

    time_t now = time(NULL);
    time_t threshold_time = now - (days_threshold * 24 * 60 * 60);

    size_t total = ctx->size;
    size_t active = 0;
    size_t expired = 0;

    for (size_t i = 0; i < ctx->size; i++) {
        CacheEntry *entry = ctx->entries[i];

        if (entry->count >= cache_threshold) {
            active++;
        }

        if (entry->last_used < threshold_time) {
            expired++;
        }
    }

    if (total_entries) *total_entries = total;
    if (active_entries) *active_entries = active;
    if (expired_entries) *expired_entries = expired;
}

/* Free text backend */
static void text_backend_free(void *backend_ctx) {
    if (!backend_ctx) {
        return;
    }

    TextBackendContext *ctx = (TextBackendContext*)backend_ctx;

    /* Free all entries */
    for (size_t i = 0; i < ctx->size; i++) {
        CacheEntry *entry = ctx->entries[i];
        free(entry->source_text);
        free(entry->translated_text);
        free(entry);
    }

    free(ctx->entries);
    free(ctx->file_path);
    free(ctx);
}

/* Get backend operations */
CacheBackendOps *text_backend_get_ops(void) {
    static CacheBackendOps ops = {
        .lookup = text_backend_lookup,
        .add = text_backend_add,
        .update_count = text_backend_update_count,
        .update_translation = text_backend_update_translation,
        .save = text_backend_save,
        .cleanup = text_backend_cleanup,
        .stats = text_backend_stats,
        .free_backend = text_backend_free
    };
    return &ops;
}
