#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <stddef.h>
#include <stdbool.h>

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

    /* Translation cache settings */
    char *cache_file;        /* Path to trans_dictionary.txt (default: ./trans_dictionary.txt) */
    int cache_threshold;     /* Minimum count to use cache (default: 5) */
    bool cache_cleanup_enabled;  /* Enable automatic cleanup (default: true) */
    int cache_cleanup_days;  /* Cleanup entries older than N days (default: 30) */
} Config;

/* Load configuration from file */
Config *load_config(const char *config_path, const char *prompt_prefix_path, const char *system_role_path);

/* Free configuration structure */
void free_config(Config *config);

/* Validate configuration */
int validate_config(const Config *config);

#endif /* CONFIG_LOADER_H */
