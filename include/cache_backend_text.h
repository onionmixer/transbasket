#ifndef CACHE_BACKEND_TEXT_H
#define CACHE_BACKEND_TEXT_H

#include "trans_cache.h"

/* Text backend specific context */
typedef struct {
    CacheEntry **entries;   /* Dynamic array of cache entries */
    size_t size;            /* Current number of entries */
    size_t capacity;        /* Allocated capacity */
    char *file_path;        /* Path to JSONL cache file */
    int next_id;            /* Next ID to assign */
} TextBackendContext;

/* Initialize text (JSONL) backend
 * Parameters:
 *   - file_path: Path to JSONL cache file
 * Returns: Initialized cache backend or NULL on error
 */
TransCache *text_backend_init(const char *file_path);

/* Get text backend operations */
CacheBackendOps *text_backend_get_ops(void);

#endif /* CACHE_BACKEND_TEXT_H */
