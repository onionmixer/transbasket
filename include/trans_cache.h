#ifndef TRANS_CACHE_H
#define TRANS_CACHE_H

#include <stddef.h>
#include <time.h>
#include <pthread.h>
#include "config_loader.h"  /* For CacheBackendType */

/* Forward declaration */
typedef struct TransCache TransCache;

/* Cache entry structure */
typedef struct {
    int id;
    char hash[65];          /* SHA256 hex string (64 chars + null) */
    char from_lang[4];      /* ISO 639-2 language code */
    char to_lang[4];        /* ISO 639-2 language code */
    char *source_text;      /* Original text */
    char *translated_text;  /* Translated text */
    int count;              /* Number of times this translation was requested */
    time_t last_used;       /* Last access timestamp (Unix time) */
    time_t created_at;      /* Creation timestamp (Unix time) */
} CacheEntry;

/* Cache backend operations interface */
typedef struct {
    /* Lookup cache entry by language pair and text */
    CacheEntry* (*lookup)(void *backend_ctx, const char *from_lang,
                          const char *to_lang, const char *text);

    /* Add new cache entry */
    int (*add)(void *backend_ctx, const char *from_lang, const char *to_lang,
               const char *source_text, const char *translated_text);

    /* Update cache entry count (increment) */
    int (*update_count)(void *backend_ctx, CacheEntry *entry);

    /* Update cache entry translation (reset count to 1) */
    int (*update_translation)(void *backend_ctx, CacheEntry *entry,
                              const char *new_translation);

    /* Save cache (persist to storage) */
    int (*save)(void *backend_ctx);

    /* Cleanup old cache entries (older than days_threshold) */
    int (*cleanup)(void *backend_ctx, int days_threshold);

    /* Get cache statistics */
    void (*stats)(void *backend_ctx, size_t *total_entries,
                  size_t *active_entries, size_t *expired_entries,
                  int cache_threshold, int days_threshold);

    /* Free backend resources */
    void (*free_backend)(void *backend_ctx);
} CacheBackendOps;

/* Translation cache structure (backend-agnostic) */
struct TransCache {
    CacheBackendType type;        /* Backend type (text, sqlite, mongodb, redis) */
    void *backend_ctx;            /* Backend-specific context */
    CacheBackendOps *ops;         /* Backend operations */
    pthread_rwlock_t lock;        /* Read-write lock for thread safety */
};

/* ============================================================================
 * Public API (backend-agnostic)
 * ============================================================================ */

/* Initialize translation cache with specified backend type
 * Parameters:
 *   - type: Backend type (CACHE_BACKEND_TEXT, CACHE_BACKEND_SQLITE, etc.)
 *   - config_path: Configuration path (file path for text, DB path for sqlite, etc.)
 *   - options: Backend-specific options (can be NULL for defaults)
 * Returns: Initialized cache or NULL on error
 */
TransCache *trans_cache_init_with_backend(CacheBackendType type,
                                          const char *config_path,
                                          void *options);

/* Initialize translation cache from file (legacy, defaults to text backend) */
TransCache *trans_cache_init(const char *file_path);

/* Lookup cache entry by language pair and text */
CacheEntry *trans_cache_lookup(TransCache *cache,
                               const char *from_lang,
                               const char *to_lang,
                               const char *text);

/* Add new cache entry */
int trans_cache_add(TransCache *cache,
                   const char *from_lang,
                   const char *to_lang,
                   const char *source_text,
                   const char *translated_text);

/* Update cache entry count (increment) */
int trans_cache_update_count(TransCache *cache, CacheEntry *entry);

/* Update cache entry translation (reset count to 1) */
int trans_cache_update_translation(TransCache *cache,
                                   CacheEntry *entry,
                                   const char *new_translation);

/* Save cache to storage */
int trans_cache_save(TransCache *cache);

/* Cleanup old cache entries (older than days_threshold) */
int trans_cache_cleanup(TransCache *cache, int days_threshold);

/* Get cache statistics */
void trans_cache_stats(TransCache *cache,
                      size_t *total_entries,
                      size_t *active_entries,
                      size_t *expired_entries,
                      int cache_threshold,
                      int days_threshold);

/* Free translation cache */
void trans_cache_free(TransCache *cache);

/* Helper function to calculate SHA256 hash for cache key */
void trans_cache_calculate_hash(const char *from_lang,
                                const char *to_lang,
                                const char *text,
                                char *hash_out);

#endif /* TRANS_CACHE_H */
