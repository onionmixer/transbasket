#ifndef TRANS_CACHE_H
#define TRANS_CACHE_H

#include <stddef.h>
#include <time.h>
#include <pthread.h>

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

/* Translation cache structure */
typedef struct {
    CacheEntry **entries;   /* Dynamic array of cache entries */
    size_t size;            /* Current number of entries */
    size_t capacity;        /* Allocated capacity */
    char *file_path;        /* Path to cache file */
    pthread_rwlock_t lock;  /* Read-write lock for thread safety */
    int next_id;            /* Next ID to assign */
} TransCache;

/* Initialize translation cache from file */
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

/* Save cache to file */
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

#endif /* TRANS_CACHE_H */
