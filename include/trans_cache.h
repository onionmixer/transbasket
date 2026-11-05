#ifndef TRANS_CACHE_H
#define TRANS_CACHE_H

#include <stddef.h>
#include <time.h>
#include <pthread.h>
#include "config_loader.h"  /* For CacheBackendType */
#include "utils.h"
#include "translation_control.h"

/* Forward declaration */
typedef struct TransCache TransCache;

/* source_hint metadata of a from=auto entry (PLAN_SOURCE_HINT_METADATA.md).
 * origin 0 means "no hint". */
#define SOURCE_HINT_ORIGIN_NONE   0
#define SOURCE_HINT_ORIGIN_SCRIPT 's'
#define SOURCE_HINT_ORIGIN_MODEL  'm'

typedef struct {
    char lang[4];           /* canonical code (ISO_639_2_CODES) or "und" */
    bool mixed;
    char origin;            /* SOURCE_HINT_ORIGIN_* */
    int  version;           /* version of the rule set that produced it, >= 1 */
} SourceHint;

/* Cache entry structure */
typedef struct {
    int id;
    char hash[65];          /* SHA256 hex string (64 chars + null) */
    char from_lang[SOURCE_LANGUAGE_SIZE]; /* 3-letter code, or source mode auto */
    char to_lang[4];        /* 3-letter code: ISO 639-2, or zhs/zht */
    char *source_text;      /* Original text */
    char *translated_text;  /* Translated text */
    int count;              /* Number of times this translation was requested */
    time_t last_used;       /* Last access timestamp (Unix time) */
    time_t created_at;      /* Creation timestamp (Unix time) */
    SourceHint hint;        /* from=auto only; origin 0 = none */
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

    /* Persist an engine result and (optionally) its hint as one operation; see
     * trans_cache_store_result(). Called with the cache write lock held. */
    int (*store_result)(void *backend_ctx, const char *from_lang, const char *to_lang,
                        const char *source_text, const char *translated_text,
                        CacheEntry *cached, const SourceHint *hint,
                        const TranslationControl *control);

    /* Merge a hint into the existing auto row `hash` by priority, leaving the
     * row itself untouched. 1 = replaced, 0 = kept / no such auto row,
     * -1 = error. Called with the cache write lock held. */
    int (*merge_hint)(void *backend_ctx, const char *hash, const SourceHint *hint);
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

/* Lookup cache entry by language pair and text.
 *
 * OWNERSHIP: the returned entry is a caller-owned DEEP COPY of the stored
 * record (all backends). Release it with trans_cache_entry_free(). Holding it
 * is safe after the cache lock is dropped, because no other thread can touch
 * the copy. Backends locate the stored record through entry->hash, so
 * trans_cache_update_count()/_update_translation() still work with the copy.
 *
 * Returns NULL when the text is not cached. */
CacheEntry *trans_cache_lookup(TransCache *cache,
                               const char *from_lang,
                               const char *to_lang,
                               const char *text);

/* Free an entry returned by trans_cache_lookup(). Safe with NULL. */
void trans_cache_entry_free(CacheEntry *entry);

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

/* Persist an engine result (the owner's only cache write) together with its
 * source_hint, under ONE acquisition of the write lock.
 *   - cached: the caller's lookup copy, or NULL. The parent mutation is the
 *     same as before: NULL -> add; same translation -> update_count;
 *     different -> update_translation. If the stored row vanished meanwhile
 *     (the update matched 0 rows) the row is added again.
 *   - hint: written only for from_lang "auto", only when valid, only after the
 *     parent mutation succeeded, and only if it does not lower the stored
 *     hint's priority (model > script; higher version wins; same-version model
 *     replaces). A failed hint write never fails the call.
 *   - control: checked after the lock is acquired and again before commit; an
 *     expired deadline writes nothing. NULL means no deadline.
 * The caller's copy is updated only after the write succeeded.
 * Returns 0 when the parent row was persisted, -1 otherwise. */
int trans_cache_store_result(TransCache *cache,
                             const char *from_lang,
                             const char *to_lang,
                             const char *source_text,
                             const char *translated_text,
                             CacheEntry *cached,
                             const SourceHint *hint,
                             const TranslationControl *control);

/* Merge `hint` into the existing auto row `hash` (see the merge_hint op).
 * Invalid hints are rejected (-1). */
int trans_cache_merge_hint(TransCache *cache, const char *hash, const SourceHint *hint);

/* Import one entry (cache_tool migrate) under one lock: if the destination has
 * no row for the entry's key, its translation and hint are added; otherwise
 * the DESTINATION translation is kept and only the hint is merged by priority.
 * *existed tells which case applied; *hint_replaced is 1 when a hint was
 * written. Returns 0 on success, -1 on failure. */
int trans_cache_import_entry(TransCache *cache, const CacheEntry *src, bool *existed, int *hint_replaced);

/* true if lang is a canonical stored code: a lowercase entry of
 * ISO_639_2_CODES or "und". */
bool source_hint_lang_canonical(const char *lang);

/* true if hint is complete and valid for storage. */
bool source_hint_valid(const SourceHint *hint);

/* true if `incoming` may replace `stored` (stored may be "no hint"). */
bool source_hint_replaces(const SourceHint *incoming, const SourceHint *stored);

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
