/**
 * Translation cache module for transbasket.
 * Backend-agnostic API with pluggable backend implementations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
    (void)options;  /* Not used yet */

    switch (type) {
        case CACHE_BACKEND_TEXT:
            return text_backend_init(config_path);

        case CACHE_BACKEND_SQLITE:
            return sqlite_backend_init(config_path);

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
    if (!cache || !cache->ops || !cache->ops->add) {
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
