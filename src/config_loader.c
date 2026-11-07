/**
 * Configuration loader module for transbasket.
 * Loads and validates configuration from transbasket.conf file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdbool.h>
#include <regex.h>
#include <libgen.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include "config_loader.h"
#include "utils.h"
#include "listen_address.h"

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

/* Parse a single configuration line. Returns 1 for KEY=value, 0 for an empty
 * line or comment, -1 if it does not parse, -2 if the key or the value does not
 * fit its buffer (it used to be copied anyway: a stack buffer overflow), -3 if
 * the value starts with a quote but does not end with the same quote - an inline
 * comment or a stray character after the closing quote, which used to become part
 * of the value (PLAN_OPEN_ISSUES.md H). For -3 the key is filled in when it fits. */
static int parse_config_line(const char *line, char *key, size_t key_size, char *value, size_t value_size) {
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

    size_t key_len = (size_t)(matches[1].rm_eo - matches[1].rm_so);
    size_t value_len = (size_t)(matches[3].rm_eo - matches[3].rm_so);

    /* Judged on the right-hand side as written, before any quote is removed. */
    const char *rhs = trimmed + matches[1].rm_eo + 1;
    size_t rhs_len = strlen(rhs);
    if (rhs_len > 0 && (rhs[0] == '"' || rhs[0] == '\'') && rhs[rhs_len - 1] != rhs[0]) {
        if (key_len < key_size) {
            memcpy(key, trimmed + matches[1].rm_so, key_len);
            key[key_len] = '\0';
        } else {
            key[0] = '\0';
        }
        return -3;
    }

    if (key_len >= key_size || value_len >= value_size) {
        return -2;
    }

    /* Extract key */
    memcpy(key, trimmed + matches[1].rm_so, key_len);
    key[key_len] = '\0';

    /* Extract value */
    memcpy(value, trimmed + matches[3].rm_so, value_len);
    value[value_len] = '\0';

    return 1;
}

/* Read one physical line. Keeps at most size-1 bytes (NUL-terminated, without the
 * '\n'), and feeds every byte, kept or not, to the LISTEN recognizer. fgets() could
 * not tell a NUL byte from the end of the data, so a line holding a NUL used to be
 * taken for an overlong one and the *next* line was discarded with it.
 * Returns false at end of file with nothing read. */
static bool read_config_line(FILE *fp, char *buf, size_t size, bool *too_long, bool *has_nul,
                             ListenScan *scan) {
    size_t kept = 0, total = 0;
    int ch;
    *too_long = false;
    *has_nul = false;
    listen_scan_init(scan);
    while ((ch = fgetc(fp)) != EOF && ch != '\n') {
        char c = (char)ch;
        listen_scan_feed(scan, &c, 1);
        total++;
        if (ch == 0) {
            *has_nul = true;
        }
        if (kept + 1 < size) {
            buf[kept++] = c;
        } else {
            *too_long = true;
        }
    }
    buf[kept] = '\0';
    return ch != EOF || total > 0;
}

/* Load text file content */
static char *load_text_file(const char *file_path, const char *file_description) {
    FILE *fp = fopen(file_path, "r");
    if (!fp) {
        LOG_INFO("Error: %s file not found: %s", file_description, file_path);
        return NULL;
    }

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        LOG_INFO("Error: %s file is empty", file_description);
        fclose(fp);
        return NULL;
    }

    /* Allocate buffer */
    char *buffer = malloc(file_size + 1);
    if (!buffer) {
        LOG_INFO("Error: Memory allocation failed");
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
        LOG_INFO("Error: %s is empty after trimming", file_description);
        free(result);
        return NULL;
    }

    return result;
}

/* Load prompt prefix from file */
static char *load_prompt_prefix(const char *prompt_prefix_path) {
    return load_text_file(prompt_prefix_path, "Prompt prefix");
}

/* Load system role from file */
static char *load_system_role(const char *system_role_path) {
    return load_text_file(system_role_path, "System role");
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

/* true if s is a non-empty run of at most 64 [A-Za-z0-9._-] characters
 * (codex_exec.c sizes its `-c key="value"` buffers for that, CODEX_TOKEN_MAX) */
static bool is_codex_token(const char *s) {
    if (!s || !*s || strlen(s) > 64) return false;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (!isalnum(ch) && ch != '.' && ch != '_' && ch != '-') return false;
    }
    return true;
}

/* atoi-compatible int setting (utils.c atoi_saturating), warning when the text is
 * not a plain integer in int range - PLAN_OPEN_ISSUES.md F. */
static int config_int(const char *key, const char *value) {
    bool plain = true;
    int n = atoi_saturating(value, &plain);
    if (!plain) {
        LOG_INFO("Warning: %s '%s' is not a plain integer in int range; read as %d\n", key, value, n);
    }
    return n;
}

/* atof() value that must be finite: nan/inf used to reach the request JSON as null. */
static double config_finite(const char *key, const char *value, bool *bad) {
    double d = atof(value);
    if (!isfinite(d)) {
        LOG_INFO("Error: %s '%s' is not a finite number", key, value);
        *bad = true;
    }
    return d;
}

/* long from strtol() -> int without wrapping (values past INT_MAX used to turn negative). */
static int long_to_int_saturating(long n) {
    if (n > INT_MAX) return INT_MAX;
    if (n < INT_MIN) return INT_MIN;
    return (int)n;
}

/* Invalid values remain invalid instead of silently enabling unlimited work. */
static int positive_integer(const char *value) {
    if (!value || !*value) return -1;
    for (const char *p = value; *p; p++) if (*p < '0' || *p > '9') return -1;
    char *end;
    errno = 0;
    long n = strtol(value, &end, 10);
    return !errno && !*end && n > 0 && n <= INT_MAX ? (int)n : -1;
}

/* Validate configuration */
int validate_config(const Config *config) {
    if (!config) {
        return -1;
    }

    /* OPENAI_* validation is only relevant for the normal (HTTP API) backend.
     * In codex mode these settings are unused, so skip them entirely - including
     * URL syntax - so a stale OPENAI_BASE_URL does not block codex startup. */
    if (config->model_type == MODEL_TYPE_NORMAL) {
        /* CODEX_* keys are unused here, so a leftover one does not block startup. */
        if (config->codex_interface_mcp) {
            LOG_INFO("Warning: CODEX_INTERFACE \"mcp\" was removed; ignored because MODEL_TYPE is normal");
        }

        /* Validate OPENAI_BASE_URL */
        if (!config->openai_base_url || strlen(config->openai_base_url) == 0) {
            LOG_INFO( "Error: OPENAI_BASE_URL is required");
            return -1;
        }

        /* Simple URL validation */
        if (strncmp(config->openai_base_url, "http://", 7) != 0 &&
            strncmp(config->openai_base_url, "https://", 8) != 0) {
            LOG_INFO( "Error: Invalid OPENAI_BASE_URL (must start with http:// or https://)");
            return -1;
        }

        /* Validate OPENAI_MODEL */
        if (!config->openai_model || strlen(config->openai_model) == 0) {
            LOG_INFO( "Error: OPENAI_MODEL is required");
            return -1;
        }

        /* Validate OPENAI_API_KEY */
        if (!config->openai_api_key || strlen(config->openai_api_key) == 0) {
            LOG_INFO( "Error: OPENAI_API_KEY is required");
            return -1;
        }
    } else {
        /* The legacy `codex mcp-server` interface was removed (PLAN_REMOVE_LEGACY_MCP.md):
         * refuse instead of silently running exec for a config that asked for mcp. */
        if (config->codex_interface_mcp) {
            LOG_INFO("Error: CODEX_INTERFACE \"mcp\" was removed (codex-cli 0.154.0 dropped `codex mcp-server`); "
                     "use CODEX_INTERFACE=\"exec\" or delete the line");
            return -1;
        }

        /* codex mode: codex_bin must be present */
        if (!config->codex_bin || strlen(config->codex_bin) == 0) {
            LOG_INFO( "Error: CODEX_BIN is required in codex mode");
            return -1;
        }

        /* These end up inside `-c key="value"` TOML overrides for codex exec,
         * so keep them to plain identifiers. */
        if (!is_codex_token(config->codex_approval_policy)) {
            LOG_INFO("Error: CODEX_APPROVAL_POLICY must match [A-Za-z0-9._-]{1,64}");
            return -1;
        }
        if (config->codex_reasoning_effort && !is_codex_token(config->codex_reasoning_effort)) {
            LOG_INFO("Error: CODEX_REASONING_EFFORT must match [A-Za-z0-9._-]{1,64}");
            return -1;
        }
    }

    /* Validate PORT */
    if (config->port < 1 || config->port > 65535) {
        LOG_INFO( "Error: PORT must be between 1 and 65535");
        return -1;
    }

    /* Validate LISTEN: the same parser the server binds with (PLAN_LISTEN_ADDRESS.md 3.2) */
    struct sockaddr_storage listen_addr;
    ListenMode listen_mode;
    const char *listen_why = "missing value";
    if (!config->listen ||
        parse_listen_address(config->listen, config->port, &listen_addr, &listen_mode, &listen_why) != 0) {
        LOG_INFO("Error: LISTEN \"%s\" %s", config->listen ? config->listen : "", listen_why);
        return -1;
    }

    /* Validate PROMPT_PREFIX */
    if (!config->prompt_prefix || strlen(config->prompt_prefix) == 0) {
        LOG_INFO( "Error: PROMPT_PREFIX is required");
        return -1;
    }

    if (config->prompt_prefix_auto &&
        (!*config->prompt_prefix_auto ||
         strstr(config->prompt_prefix_auto, "{SOURCE_LANG}") ||
         strstr(config->prompt_prefix_auto, "{SOURCE_CODE}"))) {
        LOG_INFO("Error: PROMPT_PREFIX_AUTO_FILE must be nonempty and must not contain source language placeholders");
        return -1;
    }

    if (config->translation_total_timeout < 1 || config->translation_total_timeout > 110 ||
        config->inflight_max_keys < 0 || config->inflight_max_waiters < 1 ||
        config->inflight_max_waiters_per_key < 1) {
        LOG_INFO("Error: TRANSLATION_TOTAL_TIMEOUT must be 1..110; INFLIGHT limits must be positive integers");
        return -1;
    }

    /* Validate SYSTEM_ROLE */
    if (!config->system_role || strlen(config->system_role) == 0) {
        LOG_INFO( "Error: SYSTEM_ROLE is required");
        return -1;
    }

    return 0;
}

/* Load configuration from file */
Config *load_config(const char *config_path, const char *prompt_prefix_path, const char *system_role_path) {
    const char *default_config_path = "transbasket.conf";
    const char *local_config_path = "transbasket.local.conf";
    const char *default_system_role_path = "ROLS.txt";

    /* Track if command-line arguments were explicitly provided */
    bool prompt_prefix_from_cmdline = (prompt_prefix_path != NULL);

    /* Don't set default for prompt_prefix_path here - let config file value take precedence */

    if (!system_role_path) {
        system_role_path = default_system_role_path;
    }

    /* Resolve config path (paths are relative to the executable's directory) */
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        LOG_INFO( "Error: Could not determine executable path");
        return NULL;
    }
    exe_path[len] = '\0';

    char *resolved_config_path = NULL;
    if (config_path) {
        resolved_config_path = resolve_path(exe_path, config_path);
    } else {
        /* Neither -c nor TRANSBASKET_CONFIG was given. Prefer an untracked
         * transbasket.local.conf when it exists so that the tracked template
         * never has to carry a real endpoint or API key; fall back to the
         * template otherwise. resolve_path() uses realpath(), so a NULL result
         * here simply means the file is absent. */
        resolved_config_path = resolve_path(exe_path, local_config_path);
        if (resolved_config_path) {
            config_path = local_config_path;
        } else {
            config_path = default_config_path;
            resolved_config_path = resolve_path(exe_path, config_path);
        }
    }

    if (!resolved_config_path) {
        LOG_INFO( "Error: Could not resolve config path: %s\n", config_path);
        return NULL;
    }

    LOG_INFO("Using configuration file: %s", resolved_config_path);

    /* Open config file */
    FILE *fp = fopen(resolved_config_path, "r");
    if (!fp) {
        LOG_INFO( "Error: Configuration file not found: %s\n", resolved_config_path);
        free(resolved_config_path);
        return NULL;
    }

    /* Allocate config structure */
    Config *config = calloc(1, sizeof(Config));
    if (!config) {
        LOG_INFO("Error: Memory allocation failed");
        fclose(fp);
        free(resolved_config_path);
        return NULL;
    }

    /* Set defaults */
    config->listen = strdup("0.0.0.0");
    config->port = 8889;
    config->debug = false;
    config->temperature = 0.0;
    config->top_p = 1.0;
    config->seed = 42;
    config->stream = false;
    config->normal_unescape = true;   /* PLAN_OUT_OF_SCOPE.md B1: unchanged by default */
    config->frequency_penalty = 0.0;
    config->presence_penalty = 0.0;
    config->reasoning_effort = strdup("none");

    /* Cache defaults */
    config->cache_type = CACHE_BACKEND_TEXT;  /* Default to text (JSONL) backend */
    config->cache_type_str = strdup("text");
    config->cache_file = strdup("./trans_dictionary.txt");
    config->cache_sqlite_path = strdup("./trans_cache.db");
    config->cache_sqlite_journal_mode = strdup("WAL");
    config->cache_sqlite_sync = strdup("NORMAL");
    config->cache_threshold = 5;
    config->translation_total_timeout = 110;
    config->source_hint_mode = SOURCE_HINT_SCRIPT;
    config->inflight_max_waiters = 256;
    config->inflight_max_waiters_per_key = 64;
    config->cache_cleanup_enabled = true;
    config->cache_cleanup_days = 60;

    /* Prompt and language configuration defaults */
    config->prompt_prefix_file = strdup("PROMPT_PREFIX.txt");
    config->language_table_file = strdup("language_tables.conf");

    /* Translation backend defaults */
    config->model_type = MODEL_TYPE_NORMAL;
    config->model_type_str = strdup("normal");

    /* codex backend defaults */
    config->codex_interface_mcp = false;
    config->codex_bin = strdup("codex");
    config->codex_model = NULL;             /* NULL = codex default model */
    config->codex_sandbox = strdup("read-only");
    config->codex_approval_policy = strdup("never");
    config->codex_reasoning_effort = NULL;  /* NULL = not injected */
    config->codex_run_dir = strdup("/var/lib/transbasket/run");
    config->codex_timeout = 100;
    config->codex_max_concurrency = 0;      /* 0 = use MAX_WORKERS */
    config->codex_allow_unsafe = false;

    /* Parse config file */
    _Static_assert(MAX_LINE_LENGTH == 1024, "the overlong-line message says 1023 bytes");
    char line[MAX_LINE_LENGTH];
    char key[MAX_VALUE_LENGTH];
    char value[MAX_VALUE_LENGTH];
    int line_num = 0;
    /* A line that tries to set LISTEN but is not read as LISTEN=<value> must not leave
     * the 0.0.0.0 default in place (PLAN_LISTEN_ADDRESS.md 3.3): it stops startup. */
    bool listen_line_bad = false;
    bool quote_line_bad = false;   /* PLAN_OPEN_ISSUES.md H / Q8 */
    bool number_bad = false;       /* PLAN_OPEN_ISSUES.md F: nan/inf */

    ListenScan listen_scan;
    bool too_long, has_nul;
    while (read_config_line(fp, line, sizeof(line), &too_long, &has_nul, &listen_scan)) {
        line_num++;
        /* A physical line longer than the buffer is skipped whole (its remainder is
         * never parsed as a line of its own); so is a line holding a NUL byte. */
        if (too_long || has_nul) {
            const char *what = too_long ? "is longer than 1023 bytes" : "contains a NUL byte";
            if (listen_scan_matched(&listen_scan)) {
                LOG_INFO("Error: line %d sets LISTEN but %s", line_num, what);
                listen_line_bad = true;
            } else {
                LOG_INFO("Warning: line %d %s and was ignored\n", line_num, what);
            }
            continue;
        }
        int parse_result = parse_config_line(line, key, sizeof(key), value, sizeof(value));

        if (parse_result == -3) {
            LOG_INFO("Error: line %d: the value of %s starts with a quote but does not end with the same quote "
                     "(comments must be on their own line)", line_num, key[0] ? key : "(overlong key)");
            quote_line_bad = true;
            continue;
        }

        if (listen_scan_matched(&listen_scan) && !(parse_result == 1 && strcmp(key, "LISTEN") == 0)) {
            if (parse_result == -2) {
                LOG_INFO("Error: line %d: LISTEN value longer than %d bytes", line_num, MAX_VALUE_LENGTH - 1);
            } else {
                LOG_INFO("Error: line %d sets LISTEN but does not parse; write LISTEN=\"<address>\" "
                         "(upper case, no blanks around '=', no 'export'): %s", line_num, line);
            }
            listen_line_bad = true;
            continue;
        }

        if (parse_result == -2) {
            LOG_INFO("Warning: line %d: key or value longer than %d bytes, ignored\n", line_num, MAX_VALUE_LENGTH - 1);
            continue;
        }
        if (parse_result < 0) {
            LOG_INFO( "Warning: Failed to parse line %d: %s\n", line_num, line);
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
            config->port = config_int(key, value);
        } else if (strcmp(key, "DEBUG") == 0) {
            config->debug = (strcasecmp(value, "yes") == 0 || strcmp(value, "1") == 0);
        } else if (strcmp(key, "TEMPERATURE") == 0) {
            config->temperature = config_finite(key, value, &number_bad);
        } else if (strcmp(key, "TOP_P") == 0) {
            config->top_p = config_finite(key, value, &number_bad);
        } else if (strcmp(key, "SEED") == 0) {
            config->seed = config_int(key, value);
        } else if (strcmp(key, "NORMAL_UNESCAPE") == 0) {
            if (strcasecmp(value, "yes") == 0 || strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0) {
                config->normal_unescape = true;
            } else if (strcasecmp(value, "no") == 0 || strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0) {
                config->normal_unescape = false;
            } else {
                LOG_INFO("Warning: Invalid NORMAL_UNESCAPE '%s', using 'yes'\n", value);
                config->normal_unescape = true;
            }
        } else if (strcmp(key, "STREAM") == 0) {
            config->stream = (strcasecmp(value, "yes") == 0 || strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0);
        } else if (strcmp(key, "FREQUENCY_PENALTY") == 0) {
            config->frequency_penalty = atof(value);
            if (isnan(config->frequency_penalty)) {   /* inf still clamps to +-2 below, as before */
                LOG_INFO("Error: %s '%s' is not a number", key, value);
                number_bad = true;
            }
            /* Clamp to valid range: -2.0 to 2.0 */
            if (config->frequency_penalty < -2.0) config->frequency_penalty = -2.0;
            if (config->frequency_penalty > 2.0) config->frequency_penalty = 2.0;
        } else if (strcmp(key, "PRESENCE_PENALTY") == 0) {
            config->presence_penalty = atof(value);
            if (isnan(config->presence_penalty)) {
                LOG_INFO("Error: %s '%s' is not a number", key, value);
                number_bad = true;
            }
            /* Clamp to valid range: -2.0 to 2.0 */
            if (config->presence_penalty < -2.0) config->presence_penalty = -2.0;
            if (config->presence_penalty > 2.0) config->presence_penalty = 2.0;
        } else if (strcmp(key, "TRANS_CACHE_TYPE") == 0) {
            free(config->cache_type_str);
            config->cache_type_str = strdup(value);
            /* Parse cache type */
            if (strcasecmp(value, "text") == 0) {
                config->cache_type = CACHE_BACKEND_TEXT;
            } else if (strcasecmp(value, "sqlite") == 0) {
                config->cache_type = CACHE_BACKEND_SQLITE;
            } else if (strcasecmp(value, "mongodb") == 0) {
                config->cache_type = CACHE_BACKEND_MONGODB;
                LOG_INFO("Warning: MongoDB backend not yet implemented, using text backend\n");
                config->cache_type = CACHE_BACKEND_TEXT;
            } else if (strcasecmp(value, "redis") == 0) {
                config->cache_type = CACHE_BACKEND_REDIS;
                LOG_INFO("Warning: Redis backend not yet implemented, using text backend\n");
                config->cache_type = CACHE_BACKEND_TEXT;
            } else {
                LOG_INFO("Warning: Invalid TRANS_CACHE_TYPE '%s', using 'text'\n", value);
                config->cache_type = CACHE_BACKEND_TEXT;
                free(config->cache_type_str);
                config->cache_type_str = strdup("text");
            }
        } else if (strcmp(key, "TRANS_CACHE_FILE") == 0) {
            free(config->cache_file);
            config->cache_file = strdup(value);
        } else if (strcmp(key, "TRANS_CACHE_SQLITE_PATH") == 0) {
            free(config->cache_sqlite_path);
            config->cache_sqlite_path = strdup(value);
        } else if (strcmp(key, "TRANS_CACHE_SQLITE_JOURNAL_MODE") == 0) {
            free(config->cache_sqlite_journal_mode);
            /* Validate journal mode */
            if (strcasecmp(value, "DELETE") == 0 || strcasecmp(value, "TRUNCATE") == 0 ||
                strcasecmp(value, "PERSIST") == 0 || strcasecmp(value, "MEMORY") == 0 ||
                strcasecmp(value, "WAL") == 0 || strcasecmp(value, "OFF") == 0) {
                config->cache_sqlite_journal_mode = strdup(value);
            } else {
                LOG_INFO("Warning: Invalid TRANS_CACHE_SQLITE_JOURNAL_MODE '%s', using 'WAL'\n", value);
                config->cache_sqlite_journal_mode = strdup("WAL");
            }
        } else if (strcmp(key, "TRANS_CACHE_SQLITE_SYNC") == 0) {
            free(config->cache_sqlite_sync);
            /* Validate synchronous mode */
            if (strcasecmp(value, "OFF") == 0 || strcasecmp(value, "NORMAL") == 0 ||
                strcasecmp(value, "FULL") == 0 || strcasecmp(value, "EXTRA") == 0) {
                config->cache_sqlite_sync = strdup(value);
            } else {
                LOG_INFO("Warning: Invalid TRANS_CACHE_SQLITE_SYNC '%s', using 'NORMAL'\n", value);
                config->cache_sqlite_sync = strdup("NORMAL");
            }
        } else if (strcmp(key, "SOURCE_HINT_MODE") == 0) {
            if (strcasecmp(value, "off") == 0) {
                config->source_hint_mode = SOURCE_HINT_OFF;
            } else if (strcasecmp(value, "script") == 0) {
                config->source_hint_mode = SOURCE_HINT_SCRIPT;
            } else if (strcasecmp(value, "model") == 0) {
                config->source_hint_mode = SOURCE_HINT_MODEL;
            } else {
                LOG_INFO("Warning: Invalid SOURCE_HINT_MODE '%s', using 'script'\n", value);
                config->source_hint_mode = SOURCE_HINT_SCRIPT;
            }
        } else if (strcmp(key, "TRANSLATION_TOTAL_TIMEOUT") == 0) {
            config->translation_total_timeout = positive_integer(value);
        } else if (strcmp(key, "INFLIGHT_MAX_KEYS") == 0) {
            config->inflight_max_keys = positive_integer(value);
        } else if (strcmp(key, "INFLIGHT_MAX_WAITERS") == 0) {
            config->inflight_max_waiters = positive_integer(value);
        } else if (strcmp(key, "INFLIGHT_MAX_WAITERS_PER_KEY") == 0) {
            config->inflight_max_waiters_per_key = positive_integer(value);
        } else if (strcmp(key, "TRANS_CACHE_THRESHOLD") == 0) {
            config->cache_threshold = config_int(key, value);
            if (config->cache_threshold < 1) {
                config->cache_threshold = 5;  /* Minimum 1 */
            }
        } else if (strcmp(key, "TRANS_CACHE_CLEANUP_ENABLED") == 0) {
            config->cache_cleanup_enabled = (strcasecmp(value, "yes") == 0 || strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0);
        } else if (strcmp(key, "TRANS_CACHE_CLEANUP_DAYS") == 0) {
            /* atoi-compatible (an existing "120days" keeps meaning 120); values
             * too large for int saturate - they mean "never" rather than wrapping
             * into a cutoff that deletes everything. */
            bool plain = true;
            config->cache_cleanup_days = atoi_saturating(value, &plain);
            if (config->cache_cleanup_days <= 0) {
                config->cache_cleanup_days = 60;  /* Default */
            }
            if (!plain) {
                LOG_INFO("Warning: TRANS_CACHE_CLEANUP_DAYS '%s' is not a plain integer in int range; using %d\n",
                         value, config->cache_cleanup_days);
            }
        } else if (strcmp(key, "REASONING_EFFORT") == 0) {
            free(config->reasoning_effort);
            /* Validate reasoning effort value */
            if (strcasecmp(value, "none") == 0 || strcasecmp(value, "low") == 0 ||
                strcasecmp(value, "medium") == 0 || strcasecmp(value, "high") == 0) {
                config->reasoning_effort = strdup(value);
            } else {
                LOG_INFO("Warning: Invalid REASONING_EFFORT value '%s', using 'none'\n", value);
                config->reasoning_effort = strdup("none");
            }
        } else if (strcmp(key, "PROMPT_PREFIX_FILE") == 0) {
            free(config->prompt_prefix_file);
            config->prompt_prefix_file = strdup(value);
        } else if (strcmp(key, "PROMPT_PREFIX_AUTO_FILE") == 0) {
            free(config->prompt_prefix_auto_file);
            config->prompt_prefix_auto_file = strdup(value);
        } else if (strcmp(key, "LANGUAGE_TABLE_FILE") == 0) {
            free(config->language_table_file);
            config->language_table_file = strdup(value);
        } else if (strcmp(key, "MODEL_TYPE") == 0) {
            free(config->model_type_str);
            if (strcasecmp(value, "normal") == 0) {
                config->model_type = MODEL_TYPE_NORMAL;
                config->model_type_str = strdup("normal");
            } else if (strcasecmp(value, "codex") == 0) {
                config->model_type = MODEL_TYPE_CODEX;
                config->model_type_str = strdup("codex");
            } else {
                LOG_INFO("Warning: Invalid MODEL_TYPE '%s', using 'normal'\n", value);
                config->model_type = MODEL_TYPE_NORMAL;
                config->model_type_str = strdup("normal");
            }
        } else if (strcmp(key, "CODEX_INTERFACE") == 0) {
            /* exec is the only interface. The last line wins, as before: a later
             * exec or invalid value clears an earlier mcp. */
            config->codex_interface_mcp = (strcasecmp(value, "mcp") == 0);
            if (strcasecmp(value, "exec") != 0 && !config->codex_interface_mcp) {
                LOG_INFO("Warning: Invalid CODEX_INTERFACE '%s', using 'exec'\n", value);
            }
        } else if (strcmp(key, "CODEX_BIN") == 0) {
            free(config->codex_bin);
            config->codex_bin = strdup(value);
        } else if (strcmp(key, "CODEX_MODEL") == 0) {
            free(config->codex_model);
            config->codex_model = strdup(value);
        } else if (strcmp(key, "CODEX_SANDBOX") == 0) {
            free(config->codex_sandbox);
            config->codex_sandbox = strdup(value);
        } else if (strcmp(key, "CODEX_APPROVAL_POLICY") == 0) {
            free(config->codex_approval_policy);
            config->codex_approval_policy = strdup(value);
        } else if (strcmp(key, "CODEX_REASONING_EFFORT") == 0) {
            free(config->codex_reasoning_effort);
            config->codex_reasoning_effort = strdup(value);
        } else if (strcmp(key, "CODEX_RUN_DIR") == 0) {
            free(config->codex_run_dir);
            config->codex_run_dir = strdup(value);
        } else if (strcmp(key, "CODEX_TIMEOUT") == 0) {
            char *endptr = NULL;
            long t = strtol(value, &endptr, 10);
            if (endptr == value || *endptr != '\0' || t <= 0) {
                LOG_INFO("Warning: Invalid CODEX_TIMEOUT '%s', using 100\n", value);
                config->codex_timeout = 100;
            } else {
                config->codex_timeout = long_to_int_saturating(t);
            }
        } else if (strcmp(key, "CODEX_MAX_CONCURRENCY") == 0) {
            char *endptr = NULL;
            long c = strtol(value, &endptr, 10);
            if (endptr == value || *endptr != '\0' || c < 1) {
                config->codex_max_concurrency = 0;  /* 0 = use MAX_WORKERS */
            } else {
                config->codex_max_concurrency = long_to_int_saturating(c);
            }
        } else if (strcmp(key, "CODEX_ALLOW_UNSAFE") == 0) {
            config->codex_allow_unsafe = (strcasecmp(value, "yes") == 0 ||
                                          strcmp(value, "1") == 0 ||
                                          strcasecmp(value, "true") == 0);
        } else if (strcmp(key, "CODEX_IGNORE_USER_CONFIG") == 0) {
            config->codex_ignore_user_config = (strcasecmp(value, "yes") == 0 ||
                                                strcmp(value, "1") == 0 ||
                                                strcasecmp(value, "true") == 0);
        }
    }

    fclose(fp);

    if (listen_line_bad || quote_line_bad || number_bad) {
        if (listen_line_bad) {
            LOG_INFO("Error: refusing to start with the default LISTEN address while a LISTEN line is unreadable");
        }
        free(resolved_config_path);
        free_config(config);
        return NULL;
    }

    /* Load prompt prefix - use command line arg if provided, otherwise use config file setting */
    const char *actual_prompt_path = prompt_prefix_from_cmdline ? prompt_prefix_path : config->prompt_prefix_file;
    char *resolved_prompt_path = resolve_path(resolved_config_path, actual_prompt_path);

    if (!resolved_prompt_path) {
        LOG_INFO( "Error: Could not resolve prompt prefix path: %s\n", actual_prompt_path ? actual_prompt_path : "(unset)");
        free(resolved_config_path);
        free_config(config);
        return NULL;
    }

    config->prompt_prefix = load_prompt_prefix(resolved_prompt_path);
    free(resolved_prompt_path);

    if (!config->prompt_prefix) {
        free(resolved_config_path);
        free_config(config);
        return NULL;
    }

    if (config->prompt_prefix_auto_file) {
        char *auto_path = resolve_path(resolved_config_path, config->prompt_prefix_auto_file);
        config->prompt_prefix_auto = auto_path
            ? load_text_file(auto_path, "Automatic source prompt") : NULL;
        free(auto_path);
        if (!config->prompt_prefix_auto) {
            LOG_INFO("Error: Could not load PROMPT_PREFIX_AUTO_FILE: %s", config->prompt_prefix_auto_file);
            free(resolved_config_path);
            free_config(config);
            return NULL;
        }
    }

    /* Load system role */
    char *resolved_system_role_path = resolve_path(resolved_config_path, system_role_path);
    free(resolved_config_path);

    if (!resolved_system_role_path) {
        LOG_INFO( "Error: Could not resolve system role path: %s\n", system_role_path);
        free_config(config);
        return NULL;
    }

    config->system_role = load_system_role(resolved_system_role_path);
    free(resolved_system_role_path);

    if (!config->system_role) {
        free_config(config);
        return NULL;
    }

    /* codex backend: security hard-coercion and timeout clamp.
     * Untrusted text is translated, so force the safe sandbox/approval values
     * unless the operator explicitly opted into unsafe values. */
    if (config->model_type == MODEL_TYPE_CODEX) {
        if (!config->codex_allow_unsafe) {
            if (config->codex_sandbox && strcmp(config->codex_sandbox, "read-only") != 0) {
                LOG_INFO("Warning: CODEX_SANDBOX '%s' forced to 'read-only' (set CODEX_ALLOW_UNSAFE=true to override)\n",
                        config->codex_sandbox);
                free(config->codex_sandbox);
                config->codex_sandbox = strdup("read-only");
            }
            if (config->codex_approval_policy && strcmp(config->codex_approval_policy, "never") != 0) {
                LOG_INFO("Warning: CODEX_APPROVAL_POLICY '%s' forced to 'never' (set CODEX_ALLOW_UNSAFE=true to override)\n",
                        config->codex_approval_policy);
                free(config->codex_approval_policy);
                config->codex_approval_policy = strdup("never");
            }
        }

        /* Empty CODEX_MODEL / CODEX_REASONING_EFFORT mean "not set" -> NULL */
        if (config->codex_model && strlen(config->codex_model) == 0) {
            free(config->codex_model);
            config->codex_model = NULL;
        }
        if (config->codex_reasoning_effort && strlen(config->codex_reasoning_effort) == 0) {
            free(config->codex_reasoning_effort);
            config->codex_reasoning_effort = NULL;
        }

        /* Keep codex timeout below the MHD connection timeout (120s) so a valid
         * codex response is not cut off by the idle-connection timeout. */
        if (config->codex_timeout >= 120) {
            LOG_INFO("Warning: CODEX_TIMEOUT %d >= MHD timeout 120, clamped to 110\n",
                    config->codex_timeout);
            config->codex_timeout = 110;
        }
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
    free(config->prompt_prefix_auto);
    free(config->prompt_prefix_auto_file);
    free(config->system_role);
    free(config->cache_type_str);
    free(config->cache_file);
    free(config->cache_sqlite_path);
    free(config->cache_sqlite_journal_mode);
    free(config->cache_sqlite_sync);
    free(config->reasoning_effort);
    free(config->prompt_prefix_file);
    free(config->language_table_file);
    free(config->model_type_str);
    free(config->codex_bin);
    free(config->codex_model);
    free(config->codex_sandbox);
    free(config->codex_approval_policy);
    free(config->codex_reasoning_effort);
    free(config->codex_run_dir);
    free(config);
}
