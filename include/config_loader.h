#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <stddef.h>

/* Configuration structure */
typedef struct {
    char *openai_base_url;
    char *openai_model;
    char *openai_api_key;
    char *listen;
    int port;
    char *prompt_prefix;
} Config;

/* Load configuration from file */
Config *load_config(const char *config_path, const char *prompt_prefix_path);

/* Free configuration structure */
void free_config(Config *config);

/* Validate configuration */
int validate_config(const Config *config);

#endif /* CONFIG_LOADER_H */
