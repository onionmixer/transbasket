#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <stddef.h>
#include <stdbool.h>

/* Cache backend type enumeration */
typedef enum {
    CACHE_BACKEND_TEXT = 0,    /* JSONL file-based cache (default) */
    CACHE_BACKEND_SQLITE,      /* SQLite database cache */
    CACHE_BACKEND_MONGODB,     /* MongoDB cache (future) */
    CACHE_BACKEND_REDIS        /* Redis cache (future) */
} CacheBackendType;

/* Configuration structure */
typedef struct {
    char *openai_base_url;
    char *openai_model;
    char *openai_api_key;
    char *listen;
    int port;
    char *prompt_prefix;
    char *system_role;       /* Content from ROLS.txt */
    bool debug;
    double temperature;      /* Default: 0 */
    double top_p;            /* Default: 1.0 */
    int seed;                /* Default: 42 */
    bool stream;             /* Default: false */
    double frequency_penalty; /* Default: 0.0, range: -2.0 to 2.0 */
    double presence_penalty;  /* Default: 0.0, range: -2.0 to 2.0 */
    char *reasoning_effort;  /* Default: "none", options: "none", "low", "medium", "high" */

    /* Translation cache settings */
    CacheBackendType cache_type;  /* Cache backend type (default: CACHE_BACKEND_TEXT) */
    char *cache_type_str;         /* Cache type as string for logging */

    /* Text backend settings */
    char *cache_file;        /* Path to trans_dictionary.txt (default: ./trans_dictionary.txt) */

    /* SQLite backend settings */
    char *cache_sqlite_path;        /* Path to SQLite database (default: ./trans_cache.db) */
    char *cache_sqlite_journal_mode; /* Journal mode: WAL, DELETE, etc. (default: WAL) */
    char *cache_sqlite_sync;        /* Synchronous mode: FULL, NORMAL, OFF (default: NORMAL) */

    /* Common cache settings (applies to all backends) */
    int cache_threshold;     /* Minimum count to use cache (default: 5) */
    bool cache_cleanup_enabled;  /* Enable automatic cleanup (default: true) */
    int cache_cleanup_days;  /* Cleanup entries older than N days (default: 60) */
} Config;

/* Load configuration from file */
Config *load_config(const char *config_path, const char *prompt_prefix_path, const char *system_role_path);

/* Free configuration structure */
void free_config(Config *config);

/* Validate configuration */
int validate_config(const Config *config);

#endif /* CONFIG_LOADER_H */
