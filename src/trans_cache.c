/**
 * Translation cache module for transbasket.
 * Backend-agnostic API with pluggable backend implementations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <openssl/evp.h>
#include "trans_cache.h"
#include "cache_backend_text.h"
#include "cache_backend_sqlite.h"
#include "utils.h"

/* Calculate SHA256 hash for cache key (public utility).
 * Uses the OpenSSL 3.0 EVP API; the digest input ("from|to|text") and therefore
 * the resulting hex key are byte-identical to the legacy SHA256_* version, so
 * existing cache databases stay valid. hash_out must hold 65 bytes. */
void trans_cache_calculate_hash(const char *from_lang,
                                const char *to_lang,
                                const char *text,
                                char *hash_out) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    hash_out[0] = '\0';

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return;
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, from_lang, strlen(from_lang)) != 1 ||
        EVP_DigestUpdate(ctx, "|", 1) != 1 ||
        EVP_DigestUpdate(ctx, to_lang, strlen(to_lang)) != 1 ||
        EVP_DigestUpdate(ctx, "|", 1) != 1 ||
        EVP_DigestUpdate(ctx, text, strlen(text)) != 1 ||
        EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return;
    }
    EVP_MD_CTX_free(ctx);

    /* Convert to hex string */
    for (unsigned int i = 0; i < hash_len; i++) {
        sprintf(hash_out + (i * 2), "%02x", hash[i]);
    }
    hash_out[hash_len * 2] = '\0';
}

/* Initialize translation cache with specified backend */
TransCache *trans_cache_init_with_backend(CacheBackendType type,
                                          const char *config_path,
                                          void *options) {
    switch (type) {
        case CACHE_BACKEND_TEXT:
            return text_backend_init(config_path);

        case CACHE_BACKEND_SQLITE:
            return sqlite_backend_init(config_path, (const SqliteCacheOptions *)options);

        case CACHE_BACKEND_MONGODB:
            LOG_INFO("MongoDB backend not yet implemented, using text backend\n");
            return text_backend_init(config_path);

        case CACHE_BACKEND_REDIS:
            LOG_INFO("Redis backend not yet implemented, using text backend\n");
            return text_backend_init(config_path);

        default:
            LOG_INFO("Unknown backend type %d, using text backend\n", type);
            return text_backend_init(config_path);
    }
}

/* Initialize translation cache (legacy, defaults to text backend) */
TransCache *trans_cache_init(const char *file_path) {
    return trans_cache_init_with_backend(CACHE_BACKEND_TEXT, file_path, NULL);
}

/* Lookup cache entry */
CacheEntry *trans_cache_lookup(TransCache *cache,
                               const char *from_lang,
                               const char *to_lang,
                               const char *text) {
    if (!cache || !cache->ops || !cache->ops->lookup) {
        return NULL;
    }

    /* WRITE lock, even though this is a "read": the backends mutate shared
     * state while looking up - the sqlite backend drives a single shared
     * prepared statement (reset/bind/step/column), and the text backend
     * refreshes last_used. Running these concurrently under a read lock
     * corrupts the statement and crashes (sqlite3_column_text -> NULL). */
    pthread_rwlock_wrlock(&cache->lock);
    CacheEntry *result = cache->ops->lookup(cache->backend_ctx, from_lang, to_lang, text);
    pthread_rwlock_unlock(&cache->lock);

    return result;
}

/* Free an entry handed back by trans_cache_lookup().
 * Every backend returns a caller-owned deep copy, so each successful lookup
 * must be paired with exactly one call to this. Safe to call with NULL. */
void trans_cache_entry_free(CacheEntry *entry) {
    if (!entry) {
        return;
    }
    free(entry->source_text);
    free(entry->translated_text);
    free(entry);
}

/* Add new cache entry */
int trans_cache_add(TransCache *cache,
                   const char *from_lang,
                   const char *to_lang,
                   const char *source_text,
                   const char *translated_text) {
    if (!cache || !cache->ops || !cache->ops->add || !from_lang || !to_lang ||
        (strlen(from_lang) != 3 && strcmp(from_lang, "auto") != 0) || strlen(to_lang) != 3) {
        return -1;
    }

    pthread_rwlock_wrlock(&cache->lock);
    int result = cache->ops->add(cache->backend_ctx, from_lang, to_lang,
                                 source_text, translated_text);
    pthread_rwlock_unlock(&cache->lock);

    return result;
}

/* Update cache entry count */
int trans_cache_update_count(TransCache *cache, CacheEntry *entry) {
    if (!cache || !cache->ops || !cache->ops->update_count) {
        return -1;
    }

    pthread_rwlock_wrlock(&cache->lock);
    int result = cache->ops->update_count(cache->backend_ctx, entry);
    pthread_rwlock_unlock(&cache->lock);

    return result;
}

/* Update cache entry translation */
int trans_cache_update_translation(TransCache *cache,
                                   CacheEntry *entry,
                                   const char *new_translation) {
    if (!cache || !cache->ops || !cache->ops->update_translation) {
        return -1;
    }

    pthread_rwlock_wrlock(&cache->lock);
    int result = cache->ops->update_translation(cache->backend_ctx, entry, new_translation);
    pthread_rwlock_unlock(&cache->lock);

    return result;
}

bool source_hint_lang_canonical(const char *lang) {
    if (!lang || strlen(lang) != 3) return false;
    for (int i = 0; i < 3; i++)
        if (lang[i] < 'a' || lang[i] > 'z') return false;      /* stored codes are lowercase */
    return strcmp(lang, "und") == 0 || validate_language_code(lang);
}

bool source_hint_valid(const SourceHint *hint) {
    return hint && (hint->origin == SOURCE_HINT_ORIGIN_SCRIPT || hint->origin == SOURCE_HINT_ORIGIN_MODEL) &&
           hint->version >= 1 && source_hint_lang_canonical(hint->lang);
}

/* model > script; within one origin the higher version wins and an equal
 * version replaces (same-version model: the latest result wins; same-version
 * script: identical value). */
bool source_hint_replaces(const SourceHint *incoming, const SourceHint *stored) {
    if (!source_hint_valid(incoming)) return false;
    if (!stored || !source_hint_valid(stored)) return true;
    int in_rank = incoming->origin == SOURCE_HINT_ORIGIN_MODEL ? 2 : 1;
    int st_rank = stored->origin == SOURCE_HINT_ORIGIN_MODEL ? 2 : 1;
    if (in_rank != st_rank) return in_rank > st_rank;
    return incoming->version >= stored->version;
}

int trans_cache_merge_hint(TransCache *cache, const char *hash, const SourceHint *hint) {
    if (!cache || !cache->ops || !cache->ops->merge_hint || !hash || !source_hint_valid(hint)) return -1;
    pthread_rwlock_wrlock(&cache->lock);
    int result = cache->ops->merge_hint(cache->backend_ctx, hash, hint);
    pthread_rwlock_unlock(&cache->lock);
    return result;
}

int trans_cache_import_entry(TransCache *cache, const CacheEntry *src, bool *existed, int *hint_replaced) {
    if (existed) *existed = false;
    if (hint_replaced) *hint_replaced = 0;
    if (!cache || !cache->ops || !cache->ops->lookup || !cache->ops->store_result || !cache->ops->merge_hint ||
        !src || !src->source_text || !src->translated_text ||
        (strlen(src->from_lang) != 3 && strcmp(src->from_lang, "auto") != 0) || strlen(src->to_lang) != 3) {
        return -1;
    }
    const SourceHint *hint = strcmp(src->from_lang, "auto") == 0 && source_hint_valid(&src->hint) ? &src->hint : NULL;

    pthread_rwlock_wrlock(&cache->lock);
    int result;
    /* The destination is looked up here, inside the lock: the source entry is
     * never used as the destination's copy. */
    CacheEntry *dest = cache->ops->lookup(cache->backend_ctx, src->from_lang, src->to_lang, src->source_text);
    if (!dest) {
        result = cache->ops->store_result(cache->backend_ctx, src->from_lang, src->to_lang, src->source_text,
                                          src->translated_text, NULL, hint, NULL);
        if (result == 0 && hint && hint_replaced) {
            CacheEntry *check = cache->ops->lookup(cache->backend_ctx, src->from_lang, src->to_lang, src->source_text);
            *hint_replaced = check && source_hint_valid(&check->hint) ? 1 : 0;
            trans_cache_entry_free(check);
        }
    } else {
        if (existed) *existed = true;
        result = 0;
        if (hint) {
            int merged = cache->ops->merge_hint(cache->backend_ctx, dest->hash, hint);
            if (merged < 0) result = -1;
            else if (hint_replaced) *hint_replaced = merged;
        }
        trans_cache_entry_free(dest);
    }
    pthread_rwlock_unlock(&cache->lock);
    return result;
}

int trans_cache_store_result(TransCache *cache,
                             const char *from_lang,
                             const char *to_lang,
                             const char *source_text,
                             const char *translated_text,
                             CacheEntry *cached,
                             const SourceHint *hint,
                             const TranslationControl *control) {
    if (!cache || !cache->ops || !cache->ops->store_result || !from_lang || !to_lang ||
        !source_text || !translated_text ||
        (strlen(from_lang) != 3 && strcmp(from_lang, "auto") != 0) || strlen(to_lang) != 3) {
        return -1;
    }
    /* Hints belong to from=auto entries only; an invalid one is dropped here
     * so no backend ever stores it. */
    if (strcmp(from_lang, "auto") != 0 || !source_hint_valid(hint)) hint = NULL;

    pthread_rwlock_wrlock(&cache->lock);
    int result = -1;
    /* The deadline is checked after the lock is acquired: waiting for the lock
     * can outlast the request budget. */
    if (translation_remaining_ms(control)) {
        result = cache->ops->store_result(cache->backend_ctx, from_lang, to_lang, source_text,
                                          translated_text, cached, hint, control);
    }
    pthread_rwlock_unlock(&cache->lock);
    return result;
}

/* Save cache to storage */
int trans_cache_save(TransCache *cache) {
    if (!cache || !cache->ops || !cache->ops->save) {
        return -1;
    }

    /* WRITE lock: the text backend rewrites its whole file here, and two
     * concurrent savers would interleave writes. */
    pthread_rwlock_wrlock(&cache->lock);
    int result = cache->ops->save(cache->backend_ctx);
    pthread_rwlock_unlock(&cache->lock);

    return result;
}

/* Cleanup old cache entries */
int trans_cache_cleanup(TransCache *cache, int days_threshold) {
    if (!cache || !cache->ops || !cache->ops->cleanup) {
        return 0;
    }

    pthread_rwlock_wrlock(&cache->lock);
    int result = cache->ops->cleanup(cache->backend_ctx, days_threshold);
    pthread_rwlock_unlock(&cache->lock);

    return result;
}

#define SECONDS_PER_DAY 86400

int64_t trans_cache_cutoff(int64_t now, int days) {
    int64_t span = (int64_t)days * SECONDS_PER_DAY;   /* |span| <= 1.86e14, cannot overflow */
    if (span > 0 && now < INT64_MIN + span) return INT64_MIN;
    if (span < 0 && now > INT64_MAX + span) return INT64_MAX;
    return now - span;
}

int trans_cache_cleanup_interval(int days) {
    int64_t interval = (int64_t)days * SECONDS_PER_DAY / 10;
    if (interval < 3600) return 3600;
    if (interval > INT_MAX - 3600) return INT_MAX - 3600;
    return (int)interval;
}

/* Get cache statistics */
void trans_cache_stats(TransCache *cache,
                      size_t *total_entries,
                      size_t *active_entries,
                      size_t *expired_entries,
                      int cache_threshold,
                      int days_threshold) {
    if (!cache || !cache->ops || !cache->ops->stats) {
        return;
    }

    /* WRITE lock: the sqlite backend steps the shared stmt_count_all here. */
    pthread_rwlock_wrlock(&cache->lock);
    cache->ops->stats(cache->backend_ctx, total_entries, active_entries,
                     expired_entries, cache_threshold, days_threshold);
    pthread_rwlock_unlock(&cache->lock);
}

/* Free translation cache */
void trans_cache_free(TransCache *cache) {
    if (!cache) {
        return;
    }

    /* Free backend resources */
    if (cache->ops && cache->ops->free_backend && cache->backend_ctx) {
        cache->ops->free_backend(cache->backend_ctx);
    }

    /* Destroy lock and free cache structure */
    pthread_rwlock_destroy(&cache->lock);
    free(cache);
}
