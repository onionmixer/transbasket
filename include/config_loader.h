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

/* Translation backend type (MODEL_TYPE) */
typedef enum {
    MODEL_TYPE_NORMAL = 0,     /* OpenAI-compatible HTTP API (default) */
    MODEL_TYPE_CODEX           /* codex CLI (CODEX_INTERFACE: exec or legacy MCP) */
} ModelType;

/* How the codex backend talks to the CLI (CODEX_INTERFACE) */
typedef enum {
    CODEX_INTERFACE_EXEC = 0,  /* `codex exec` per request (default, current CLIs) */
    CODEX_INTERFACE_MCP        /* `codex mcp-server` (legacy, codex-cli < 0.154.0) */
} CodexInterface;

/* Configuration structure */
typedef struct {
    char *openai_base_url;
    char *openai_model;
    char *openai_api_key;
    char *listen;
    int port;
    char *prompt_prefix;
    char *prompt_prefix_auto;     /* NULL = built-in automatic source instruction */
    char *prompt_prefix_auto_file; /* Optional, relative to config file */
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
    int translation_total_timeout; /* Monotonic total budget, default 110s */
    int inflight_max_keys;         /* 0 (unset) = MAX_WORKERS */
    int inflight_max_waiters;      /* default 256 */
    int inflight_max_waiters_per_key; /* default 64 */
    bool cache_cleanup_enabled;  /* Enable automatic cleanup (default: true) */
    int cache_cleanup_days;  /* Cleanup entries older than N days (default: 60) */

    /* Prompt and language configuration */
    char *prompt_prefix_file;    /* Path to prompt prefix file (default: PROMPT_PREFIX.txt) */
    char *language_table_file;   /* Path to language table file (default: language_tables.conf) */

    /* Translation backend selection (MODEL_TYPE) */
    ModelType model_type;            /* Backend type (default: MODEL_TYPE_NORMAL) */
    char *model_type_str;            /* Backend type as string for logging */

    /* codex backend settings - only used when model_type == MODEL_TYPE_CODEX */
    CodexInterface codex_interface;  /* exec (default) or legacy mcp */
    char *codex_bin;                 /* codex executable (default "codex", PATH-searched) */
    char *codex_model;               /* model override (optional, NULL = codex default) */
    char *codex_sandbox;             /* sandbox mode (forced "read-only" unless allow_unsafe) */
    char *codex_approval_policy;     /* approval policy (forced "never" unless allow_unsafe) */
    char *codex_reasoning_effort;    /* model_reasoning_effort override (optional, NULL = not set) */
    char *codex_run_dir;             /* root for per-request temp cwd (default /var/lib/transbasket/run) */
    int   codex_timeout;             /* response timeout seconds (default 100, < MHD timeout) */
    int   codex_max_concurrency;     /* max concurrent codex processes (0 = use MAX_WORKERS) */
    bool  codex_allow_unsafe;        /* allow unsafe sandbox/approval values (default false) */
    bool  codex_ignore_user_config;  /* exec: skip $CODEX_HOME/config.toml (default false) */
} Config;

/* Load configuration from file */
Config *load_config(const char *config_path, const char *prompt_prefix_path, const char *system_role_path);

/* Free configuration structure */
void free_config(Config *config);

/* Validate configuration */
int validate_config(const Config *config);

#endif /* CONFIG_LOADER_H */
