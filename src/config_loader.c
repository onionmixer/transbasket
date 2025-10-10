/**
 * Configuration loader module for transbasket.
 * Loads and validates configuration from transbasket.conf file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <regex.h>
#include <libgen.h>
#include <unistd.h>
#include <limits.h>
#include "config_loader.h"

#define MAX_LINE_LENGTH 1024
#define MAX_VALUE_LENGTH 512

/* Helper function to trim whitespace */
static char *trim_whitespace(char *str) {
    char *end;

    while (*str && (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')) {
        str++;
    }

    if (*str == 0) {
        return str;
    }

    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        end--;
    }

    *(end + 1) = '\0';
    return str;
}

/* Parse a single configuration line */
static int parse_config_line(const char *line, char *key, char *value) {
    if (!line || !key || !value) {
        return -1;
    }

    /* Skip empty lines and comments */
    char *trimmed = trim_whitespace((char *)line);
    if (strlen(trimmed) == 0 || trimmed[0] == '#') {
        return 0;
    }

    /* Match KEY="value" or KEY='value' or KEY=value */
    regex_t regex;
    regmatch_t matches[4];
    const char *pattern = "^([A-Z_]+)=([\"']?)(.+?)\\2$";

    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        return -1;
    }

    int result = regexec(&regex, trimmed, 4, matches, 0);
    regfree(&regex);

    if (result != 0) {
        return -1;
    }

    /* Extract key */
    int key_len = matches[1].rm_eo - matches[1].rm_so;
    strncpy(key, trimmed + matches[1].rm_so, key_len);
    key[key_len] = '\0';

    /* Extract value */
    int value_len = matches[3].rm_eo - matches[3].rm_so;
    strncpy(value, trimmed + matches[3].rm_so, value_len);
    value[value_len] = '\0';

    return 1;
}

/* Load prompt prefix from file */
static char *load_prompt_prefix(const char *prompt_prefix_path) {
    FILE *fp = fopen(prompt_prefix_path, "r");
    if (!fp) {
        fprintf(stderr, "Error: Prompt prefix file not found: %s\n", prompt_prefix_path);
        return NULL;
    }

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        fprintf(stderr, "Error: Prompt prefix file is empty\n");
        fclose(fp);
        return NULL;
    }

    /* Allocate buffer */
    char *buffer = malloc(file_size + 1);
    if (!buffer) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        fclose(fp);
        return NULL;
    }

    /* Read file */
    size_t read_size = fread(buffer, 1, file_size, fp);
    buffer[read_size] = '\0';
    fclose(fp);

    /* Trim whitespace */
    char *trimmed = trim_whitespace(buffer);
    char *result = strdup(trimmed);
    free(buffer);

    if (!result || strlen(result) == 0) {
        fprintf(stderr, "Error: Prompt prefix is empty after trimming\n");
        free(result);
        return NULL;
    }

    return result;
}

/* Resolve relative path from config file location */
static char *resolve_path(const char *base_path, const char *relative_path) {
    if (!relative_path) {
        return NULL;
    }

    /* If already absolute, return copy */
    if (relative_path[0] == '/') {
        return strdup(relative_path);
    }

    /* Get directory of base_path */
    char *base_copy = strdup(base_path);
    char *dir = dirname(base_copy);

    /* Build full path */
    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir, relative_path);

    /* Resolve to absolute path */
    char *resolved = realpath(full_path, NULL);
    free(base_copy);

    return resolved;
}

/* Validate configuration */
int validate_config(const Config *config) {
    if (!config) {
        return -1;
    }

    /* Validate OPENAI_BASE_URL */
    if (!config->openai_base_url || strlen(config->openai_base_url) == 0) {
        fprintf(stderr, "Error: OPENAI_BASE_URL is required\n");
        return -1;
    }

    /* Simple URL validation */
    if (strncmp(config->openai_base_url, "http://", 7) != 0 &&
        strncmp(config->openai_base_url, "https://", 8) != 0) {
        fprintf(stderr, "Error: Invalid OPENAI_BASE_URL (must start with http:// or https://)\n");
        return -1;
    }

    /* Validate OPENAI_MODEL */
    if (!config->openai_model || strlen(config->openai_model) == 0) {
        fprintf(stderr, "Error: OPENAI_MODEL is required\n");
        return -1;
    }

    /* Validate OPENAI_API_KEY */
    if (!config->openai_api_key || strlen(config->openai_api_key) == 0) {
        fprintf(stderr, "Error: OPENAI_API_KEY is required\n");
        return -1;
    }

    /* Validate PORT */
    if (config->port < 1 || config->port > 65535) {
        fprintf(stderr, "Error: PORT must be between 1 and 65535\n");
        return -1;
    }

    /* Validate LISTEN */
    if (!config->listen || strlen(config->listen) == 0) {
        fprintf(stderr, "Error: LISTEN address is required\n");
        return -1;
    }

    /* Validate PROMPT_PREFIX */
    if (!config->prompt_prefix || strlen(config->prompt_prefix) == 0) {
        fprintf(stderr, "Error: PROMPT_PREFIX is required\n");
        return -1;
    }

    return 0;
}

/* Load configuration from file */
Config *load_config(const char *config_path, const char *prompt_prefix_path) {
    const char *default_config_path = "transbasket.conf";
    const char *default_prompt_path = "PROMPT_PREFIX.txt";

    if (!config_path) {
        config_path = default_config_path;
    }

    if (!prompt_prefix_path) {
        prompt_prefix_path = default_prompt_path;
    }

    /* Resolve config path */
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        fprintf(stderr, "Error: Could not determine executable path\n");
        return NULL;
    }
    exe_path[len] = '\0';

    char *resolved_config_path = resolve_path(exe_path, config_path);
    if (!resolved_config_path) {
        fprintf(stderr, "Error: Could not resolve config path: %s\n", config_path);
        return NULL;
    }

    /* Open config file */
    FILE *fp = fopen(resolved_config_path, "r");
    if (!fp) {
        fprintf(stderr, "Error: Configuration file not found: %s\n", resolved_config_path);
        free(resolved_config_path);
        return NULL;
    }

    /* Allocate config structure */
    Config *config = calloc(1, sizeof(Config));
    if (!config) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        fclose(fp);
        free(resolved_config_path);
        return NULL;
    }

    /* Set defaults */
    config->listen = strdup("0.0.0.0");
    config->port = 8889;

    /* Parse config file */
    char line[MAX_LINE_LENGTH];
    char key[MAX_VALUE_LENGTH];
    char value[MAX_VALUE_LENGTH];
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        int parse_result = parse_config_line(line, key, value);

        if (parse_result < 0) {
            fprintf(stderr, "Warning: Failed to parse line %d: %s\n", line_num, line);
            continue;
        }

        if (parse_result == 0) {
            continue;  /* Empty line or comment */
        }

        /* Store configuration values */
        if (strcmp(key, "OPENAI_BASE_URL") == 0) {
            config->openai_base_url = strdup(value);
        } else if (strcmp(key, "OPENAI_MODEL") == 0) {
            config->openai_model = strdup(value);
        } else if (strcmp(key, "OPENAI_API_KEY") == 0) {
            config->openai_api_key = strdup(value);
        } else if (strcmp(key, "LISTEN") == 0) {
            free(config->listen);
            config->listen = strdup(value);
        } else if (strcmp(key, "PORT") == 0) {
            config->port = atoi(value);
        }
    }

    fclose(fp);

    /* Load prompt prefix */
    char *resolved_prompt_path = resolve_path(resolved_config_path, prompt_prefix_path);
    free(resolved_config_path);

    if (!resolved_prompt_path) {
        fprintf(stderr, "Error: Could not resolve prompt prefix path: %s\n", prompt_prefix_path);
        free_config(config);
        return NULL;
    }

    config->prompt_prefix = load_prompt_prefix(resolved_prompt_path);
    free(resolved_prompt_path);

    if (!config->prompt_prefix) {
        free_config(config);
        return NULL;
    }

    /* Validate configuration */
    if (validate_config(config) != 0) {
        free_config(config);
        return NULL;
    }

    return config;
}

/* Free configuration structure */
void free_config(Config *config) {
    if (!config) {
        return;
    }

    free(config->openai_base_url);
    free(config->openai_model);
    free(config->openai_api_key);
    free(config->listen);
    free(config->prompt_prefix);
    free(config);
}
