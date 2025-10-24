/**
 * Translation cache module for transbasket.
 * Implements local dictionary cache with JSONL storage format.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <openssl/sha.h>
#include <cjson/cJSON.h>
#include "trans_cache.h"
#include "utils.h"

#define INITIAL_CAPACITY 100
#define GROWTH_FACTOR 2

/* Calculate SHA256 hash for cache key */
static void calculate_cache_hash(const char *from_lang,
                                 const char *to_lang,
                                 const char *text,
                                 char *hash_out) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;

    SHA256_Init(&sha256);
    SHA256_Update(&sha256, from_lang, strlen(from_lang));
    SHA256_Update(&sha256, "|", 1);
    SHA256_Update(&sha256, to_lang, strlen(to_lang));
    SHA256_Update(&sha256, "|", 1);
    SHA256_Update(&sha256, text, strlen(text));
    SHA256_Final(hash, &sha256);

    /* Convert to hex string */
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hash_out + (i * 2), "%02x", hash[i]);
    }
    hash_out[64] = '\0';
}

/* Load cache entries from JSONL file */
static int load_cache_from_file(TransCache *cache, const char *file_path) {
    FILE *fp = fopen(file_path, "r");
    if (!fp) {
        /* File doesn't exist yet - this is OK */
        LOG_DEBUG( "Cache file not found, will create new: %s\n", file_path);
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
            LOG_DEBUG( "Warning: Failed to parse cache line, skipping");
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
            LOG_DEBUG( "Warning: Invalid cache entry format, skipping");
            cJSON_Delete(json);
            continue;
        }

        /* Allocate cache entry */
        CacheEntry *entry = calloc(1, sizeof(CacheEntry));
        if (!entry) {
            LOG_DEBUG( "Error: Memory allocation failed");
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
            LOG_DEBUG( "Error: Memory allocation failed");
            free(entry->source_text);
            free(entry->translated_text);
            free(entry);
            cJSON_Delete(json);
            break;
        }

        /* Expand capacity if needed */
        if (cache->size >= cache->capacity) {
            size_t new_capacity = cache->capacity * GROWTH_FACTOR;
            CacheEntry **new_entries = realloc(cache->entries,
                                              new_capacity * sizeof(CacheEntry *));
            if (!new_entries) {
                LOG_DEBUG( "Error: Memory reallocation failed");
                free(entry->source_text);
                free(entry->translated_text);
                free(entry);
                cJSON_Delete(json);
                break;
            }
            cache->entries = new_entries;
            cache->capacity = new_capacity;
        }

        /* Add to cache */
        cache->entries[cache->size++] = entry;
        loaded_count++;

        /* Update next_id */
        if (entry->id >= cache->next_id) {
            cache->next_id = entry->id + 1;
        }

        cJSON_Delete(json);
    }

    free(line);
    fclose(fp);

    LOG_INFO( "Loaded %d cache entries from %s\n", loaded_count, file_path);
    return loaded_count;
}

/* Initialize translation cache */
TransCache *trans_cache_init(const char *file_path) {
    if (!file_path) {
        LOG_DEBUG( "Error: NULL file path");
        return NULL;
    }

    TransCache *cache = calloc(1, sizeof(TransCache));
    if (!cache) {
        LOG_DEBUG( "Error: Memory allocation failed");
        return NULL;
    }

    /* Allocate initial capacity */
    cache->entries = malloc(INITIAL_CAPACITY * sizeof(CacheEntry *));
    if (!cache->entries) {
        LOG_DEBUG( "Error: Memory allocation failed");
        free(cache);
        return NULL;
    }

    cache->size = 0;
    cache->capacity = INITIAL_CAPACITY;
    cache->file_path = strdup(file_path);
    cache->next_id = 1;

    if (!cache->file_path) {
        LOG_DEBUG( "Error: Memory allocation failed");
        free(cache->entries);
        free(cache);
        return NULL;
    }

    /* Initialize read-write lock */
    if (pthread_rwlock_init(&cache->lock, NULL) != 0) {
        LOG_DEBUG( "Error: Failed to initialize rwlock");
        free(cache->file_path);
        free(cache->entries);
        free(cache);
        return NULL;
    }

    /* Load existing cache from file */
    load_cache_from_file(cache, file_path);

    return cache;
}

/* Lookup cache entry */
CacheEntry *trans_cache_lookup(TransCache *cache,
                               const char *from_lang,
                               const char *to_lang,
                               const char *text) {
    if (!cache || !from_lang || !to_lang || !text) {
        return NULL;
    }

    /* Calculate hash */
    char hash[65];
    calculate_cache_hash(from_lang, to_lang, text, hash);

    /* Search for entry */
    pthread_rwlock_rdlock(&cache->lock);

    CacheEntry *found = NULL;
    for (size_t i = 0; i < cache->size; i++) {
        if (strcmp(cache->entries[i]->hash, hash) == 0) {
            found = cache->entries[i];
            break;
        }
    }

    pthread_rwlock_unlock(&cache->lock);

    /* Update last_used timestamp if found */
    if (found) {
        pthread_rwlock_wrlock(&cache->lock);
        found->last_used = time(NULL);
        pthread_rwlock_unlock(&cache->lock);
    }

    return found;
}

/* Add new cache entry */
int trans_cache_add(TransCache *cache,
                   const char *from_lang,
                   const char *to_lang,
                   const char *source_text,
                   const char *translated_text) {
    if (!cache || !from_lang || !to_lang || !source_text || !translated_text) {
        return -1;
    }

    /* Allocate new entry */
    CacheEntry *entry = calloc(1, sizeof(CacheEntry));
    if (!entry) {
        LOG_DEBUG( "Error: Memory allocation failed");
        return -1;
    }

    /* Calculate hash */
    calculate_cache_hash(from_lang, to_lang, source_text, entry->hash);

    /* Fill entry data */
    entry->id = cache->next_id++;
    strncpy(entry->from_lang, from_lang, sizeof(entry->from_lang) - 1);
    strncpy(entry->to_lang, to_lang, sizeof(entry->to_lang) - 1);
    entry->source_text = strdup(source_text);
    entry->translated_text = strdup(translated_text);
    entry->count = 1;
    entry->created_at = time(NULL);
    entry->last_used = time(NULL);

    if (!entry->source_text || !entry->translated_text) {
        LOG_DEBUG( "Error: Memory allocation failed");
        free(entry->source_text);
        free(entry->translated_text);
        free(entry);
        return -1;
    }

    pthread_rwlock_wrlock(&cache->lock);

    /* Expand capacity if needed */
    if (cache->size >= cache->capacity) {
        size_t new_capacity = cache->capacity * GROWTH_FACTOR;
        CacheEntry **new_entries = realloc(cache->entries,
                                          new_capacity * sizeof(CacheEntry *));
        if (!new_entries) {
            LOG_DEBUG( "Error: Memory reallocation failed");
            pthread_rwlock_unlock(&cache->lock);
            free(entry->source_text);
            free(entry->translated_text);
            free(entry);
            return -1;
        }
        cache->entries = new_entries;
        cache->capacity = new_capacity;
    }

    /* Add to cache */
    cache->entries[cache->size++] = entry;

    pthread_rwlock_unlock(&cache->lock);

    return 0;
}

/* Update cache entry count */
int trans_cache_update_count(TransCache *cache, CacheEntry *entry) {
    if (!cache || !entry) {
        return -1;
    }

    pthread_rwlock_wrlock(&cache->lock);
    entry->count++;
    entry->last_used = time(NULL);
    pthread_rwlock_unlock(&cache->lock);

    return 0;
}

/* Update cache entry translation */
int trans_cache_update_translation(TransCache *cache,
                                   CacheEntry *entry,
                                   const char *new_translation) {
    if (!cache || !entry || !new_translation) {
        return -1;
    }

    pthread_rwlock_wrlock(&cache->lock);

    /* Free old translation */
    free(entry->translated_text);

    /* Set new translation */
    entry->translated_text = strdup(new_translation);
    if (!entry->translated_text) {
        LOG_DEBUG( "Error: Memory allocation failed");
        pthread_rwlock_unlock(&cache->lock);
        return -1;
    }

    /* Reset count to 1 */
    entry->count = 1;
    entry->last_used = time(NULL);

    pthread_rwlock_unlock(&cache->lock);

    return 0;
}

/* Save cache to file */
int trans_cache_save(TransCache *cache) {
    if (!cache || !cache->file_path) {
        return -1;
    }

    FILE *fp = fopen(cache->file_path, "w");
    if (!fp) {
        LOG_DEBUG( "Error: Failed to open cache file for writing: %s\n",
                cache->file_path);
        return -1;
    }

    pthread_rwlock_rdlock(&cache->lock);

    for (size_t i = 0; i < cache->size; i++) {
        CacheEntry *entry = cache->entries[i];

        /* Create JSON object */
        cJSON *json = cJSON_CreateObject();
        if (!json) {
            LOG_DEBUG( "Error: Failed to create JSON object");
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

    pthread_rwlock_unlock(&cache->lock);

    fclose(fp);

    // fprintf(stderr, "Saved %zu cache entries to %s\n", cache->size, cache->file_path);
    return 0;
}

/* Cleanup old cache entries */
int trans_cache_cleanup(TransCache *cache, int days_threshold) {
    if (!cache || days_threshold <= 0) {
        return 0;
    }

    time_t now = time(NULL);
    time_t threshold_time = now - (days_threshold * 24 * 60 * 60);

    pthread_rwlock_wrlock(&cache->lock);

    int removed_count = 0;
    size_t write_idx = 0;

    /* Iterate and keep only valid entries */
    for (size_t i = 0; i < cache->size; i++) {
        CacheEntry *entry = cache->entries[i];

        if (entry->last_used < threshold_time) {
            /* Expired entry - free memory */
            free(entry->source_text);
            free(entry->translated_text);
            free(entry);
            removed_count++;
        } else {
            /* Valid entry - keep it */
            cache->entries[write_idx++] = entry;
        }
    }

    cache->size = write_idx;

    pthread_rwlock_unlock(&cache->lock);

    return removed_count;
}

/* Get cache statistics */
void trans_cache_stats(TransCache *cache,
                      size_t *total_entries,
                      size_t *active_entries,
                      size_t *expired_entries,
                      int cache_threshold,
                      int days_threshold) {
    if (!cache) {
        return;
    }

    pthread_rwlock_rdlock(&cache->lock);

    time_t now = time(NULL);
    time_t threshold_time = now - (days_threshold * 24 * 60 * 60);

    size_t total = cache->size;
    size_t active = 0;
    size_t expired = 0;

    for (size_t i = 0; i < cache->size; i++) {
        CacheEntry *entry = cache->entries[i];

        if (entry->count >= cache_threshold) {
            active++;
        }

        if (entry->last_used < threshold_time) {
            expired++;
        }
    }

    pthread_rwlock_unlock(&cache->lock);

    if (total_entries) *total_entries = total;
    if (active_entries) *active_entries = active;
    if (expired_entries) *expired_entries = expired;
}

/* Free translation cache */
void trans_cache_free(TransCache *cache) {
    if (!cache) {
        return;
    }

    /* Free all entries */
    for (size_t i = 0; i < cache->size; i++) {
        CacheEntry *entry = cache->entries[i];
        free(entry->source_text);
        free(entry->translated_text);
        free(entry);
    }

    free(cache->entries);
    free(cache->file_path);
    pthread_rwlock_destroy(&cache->lock);
    free(cache);
}
