/**
 * Language table loader implementation for transbasket.
 * Loads language code to name mappings from language_tables.conf file.
 * Supports ISO 639-1 (2-letter) and ISO 639-2 (3-letter) code conversion.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <regex.h>
#include <libgen.h>
#include <unistd.h>
#include <limits.h>
#include "language_table.h"
#include "utils.h"

#define MAX_LINE_LENGTH 1024
#define MAX_VALUE_LENGTH 512
#define INITIAL_CAPACITY 512
#define INITIAL_MAPPING_CAPACITY 128

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

/**
 * Parse a single language table line.
 * Format: code="Language Name" or code='Language Name'
 *
 * @param line Input line
 * @param code Output buffer for code
 * @param name Output buffer for name
 * @return 1 on success, 0 for empty/comment lines, -1 on error
 */
static int parse_language_line(const char *line, char *code, char *name) {
    if (!line || !code || !name) {
        return -1;
    }

    /* Skip empty lines and comments */
    char *trimmed = trim_whitespace((char *)line);
    if (strlen(trimmed) == 0 || trimmed[0] == '#') {
        return 0;
    }

    /* Match code="value" or code='value' or code=value
     * Language codes can contain: a-z, A-Z, 0-9, hyphen
     */
    regex_t regex;
    regmatch_t matches[4];
    const char *pattern = "^([a-zA-Z0-9-]+)=([\"']?)(.+?)\\2$";

    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        return -1;
    }

    int result = regexec(&regex, trimmed, 4, matches, 0);
    regfree(&regex);

    if (result != 0) {
        return -1;
    }

    /* Extract code */
    int code_len = matches[1].rm_eo - matches[1].rm_so;
    if (code_len >= MAX_VALUE_LENGTH) {
        code_len = MAX_VALUE_LENGTH - 1;
    }
    strncpy(code, trimmed + matches[1].rm_so, code_len);
    code[code_len] = '\0';

    /* Extract name */
    int name_len = matches[3].rm_eo - matches[3].rm_so;
    if (name_len >= MAX_VALUE_LENGTH) {
        name_len = MAX_VALUE_LENGTH - 1;
    }
    strncpy(name, trimmed + matches[3].rm_so, name_len);
    name[name_len] = '\0';

    return 1;
}

/**
 * Parse a code mapping line.
 * Format: iso639_2:iso639_1 (e.g., "kor:ko")
 *
 * @param line Input line
 * @param iso639_2 Output buffer for 3-letter code
 * @param iso639_1 Output buffer for 2-letter code
 * @return 1 on success, 0 for empty/comment/section lines, -1 on error
 */
static int parse_mapping_line(const char *line, char *iso639_2, char *iso639_1) {
    if (!line || !iso639_2 || !iso639_1) {
        return -1;
    }

    /* Skip empty lines and comments */
    char *trimmed = trim_whitespace((char *)line);
    if (strlen(trimmed) == 0 || trimmed[0] == '#' || trimmed[0] == '[') {
        return 0;
    }

    /* Match pattern: abc:xy or abc:xyz */
    regex_t regex;
    regmatch_t matches[3];
    const char *pattern = "^([a-zA-Z]{3}):([a-zA-Z]{2,3})$";

    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        return -1;
    }

    int result = regexec(&regex, trimmed, 3, matches, 0);
    regfree(&regex);

    if (result != 0) {
        return -1;
    }

    /* Extract ISO 639-2 (3-letter) */
    int len1 = matches[1].rm_eo - matches[1].rm_so;
    strncpy(iso639_2, trimmed + matches[1].rm_so, len1);
    iso639_2[len1] = '\0';

    /* Extract ISO 639-1 (2-letter) */
    int len2 = matches[2].rm_eo - matches[2].rm_so;
    strncpy(iso639_1, trimmed + matches[2].rm_so, len2);
    iso639_1[len2] = '\0';

    return 1;
}

/* Grow mappings array capacity */
static int grow_mappings(LanguageTable *table) {
    size_t new_capacity = table->mapping_capacity * 2;
    CodeMapping *new_mappings = realloc(table->mappings, new_capacity * sizeof(CodeMapping));

    if (!new_mappings) {
        return -1;
    }

    table->mappings = new_mappings;
    table->mapping_capacity = new_capacity;
    return 0;
}

/* Resolve path relative to executable location */
static char *resolve_path(const char *relative_path) {
    if (!relative_path) {
        return NULL;
    }

    /* If already absolute, return copy */
    if (relative_path[0] == '/') {
        return strdup(relative_path);
    }

    /* Get executable path */
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        return NULL;
    }
    exe_path[len] = '\0';

    /* Get directory of executable */
    char *exe_copy = strdup(exe_path);
    char *dir = dirname(exe_copy);

    /* Build full path */
    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir, relative_path);

    /* Resolve to absolute path */
    char *resolved = realpath(full_path, NULL);
    free(exe_copy);

    return resolved;
}

/* Grow table capacity */
static int grow_table(LanguageTable *table) {
    size_t new_capacity = table->capacity * 2;
    LanguageEntry *new_entries = realloc(table->entries, new_capacity * sizeof(LanguageEntry));

    if (!new_entries) {
        return -1;
    }

    table->entries = new_entries;
    table->capacity = new_capacity;
    return 0;
}

/* Load language table from file */
LanguageTable *language_table_load(const char *file_path) {
    const char *default_path = "../language_tables.conf";
    char *resolved_path = NULL;

    if (file_path) {
        resolved_path = resolve_path(file_path);
    } else {
        resolved_path = resolve_path(default_path);
    }

    if (!resolved_path) {
        LOG_INFO("Error: Could not resolve language table path: %s",
                 file_path ? file_path : default_path);
        return NULL;
    }

    FILE *fp = fopen(resolved_path, "r");
    if (!fp) {
        LOG_INFO("Error: Language table file not found: %s", resolved_path);
        free(resolved_path);
        return NULL;
    }

    /* Allocate table structure */
    LanguageTable *table = calloc(1, sizeof(LanguageTable));
    if (!table) {
        LOG_INFO("Error: Memory allocation failed for language table");
        fclose(fp);
        free(resolved_path);
        return NULL;
    }

    /* Allocate initial entries array */
    table->entries = calloc(INITIAL_CAPACITY, sizeof(LanguageEntry));
    if (!table->entries) {
        LOG_INFO("Error: Memory allocation failed for language entries");
        free(table);
        fclose(fp);
        free(resolved_path);
        return NULL;
    }

    table->capacity = INITIAL_CAPACITY;
    table->count = 0;

    /* Allocate initial mappings array */
    table->mappings = calloc(INITIAL_MAPPING_CAPACITY, sizeof(CodeMapping));
    if (!table->mappings) {
        LOG_INFO("Error: Memory allocation failed for code mappings");
        free(table->entries);
        free(table);
        fclose(fp);
        free(resolved_path);
        return NULL;
    }

    table->mapping_capacity = INITIAL_MAPPING_CAPACITY;
    table->mapping_count = 0;

    /* Parse file */
    char line[MAX_LINE_LENGTH];
    char code[MAX_VALUE_LENGTH];
    char name[MAX_VALUE_LENGTH];
    int line_num = 0;
    bool in_mapping_section = false;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;

        /* Check for section header */
        char *trimmed = trim_whitespace((char *)line);
        if (strcmp(trimmed, "[CODE_MAPPING]") == 0) {
            in_mapping_section = true;
            continue;
        }

        /* Check for end of mapping section (next section or language entries) */
        if (in_mapping_section && trimmed[0] == '#' && strstr(trimmed, "Language Code to Name") != NULL) {
            in_mapping_section = false;
        }

        if (in_mapping_section) {
            /* Parse code mapping */
            char iso639_2[8], iso639_1[8];
            int parse_result = parse_mapping_line(line, iso639_2, iso639_1);

            if (parse_result == 1) {
                /* Grow mappings if needed */
                if (table->mapping_count >= table->mapping_capacity) {
                    if (grow_mappings(table) != 0) {
                        LOG_INFO("Error: Failed to grow code mappings");
                        language_table_free(table);
                        fclose(fp);
                        free(resolved_path);
                        return NULL;
                    }
                }

                /* Add mapping */
                table->mappings[table->mapping_count].iso639_2 = strdup(iso639_2);
                table->mappings[table->mapping_count].iso639_1 = strdup(iso639_1);

                if (!table->mappings[table->mapping_count].iso639_2 ||
                    !table->mappings[table->mapping_count].iso639_1) {
                    LOG_INFO("Error: Memory allocation failed for code mapping");
                    language_table_free(table);
                    fclose(fp);
                    free(resolved_path);
                    return NULL;
                }

                table->mapping_count++;
            }
            continue;
        }

        /* Parse language entry */
        int parse_result = parse_language_line(line, code, name);

        if (parse_result < 0) {
            LOG_DEBUG("Warning: Failed to parse language table line %d: %s", line_num, line);
            continue;
        }

        if (parse_result == 0) {
            continue;  /* Empty line or comment */
        }

        /* Grow table if needed */
        if (table->count >= table->capacity) {
            if (grow_table(table) != 0) {
                LOG_INFO("Error: Failed to grow language table");
                language_table_free(table);
                fclose(fp);
                free(resolved_path);
                return NULL;
            }
        }

        /* Add entry */
        table->entries[table->count].code = strdup(code);
        table->entries[table->count].name = strdup(name);

        if (!table->entries[table->count].code || !table->entries[table->count].name) {
            LOG_INFO("Error: Memory allocation failed for language entry");
            language_table_free(table);
            fclose(fp);
            free(resolved_path);
            return NULL;
        }

        table->count++;
    }

    fclose(fp);

    LOG_INFO("Loaded %zu language entries and %zu code mappings from %s",
             table->count, table->mapping_count, resolved_path);
    free(resolved_path);

    return table;
}

/* Free language table */
void language_table_free(LanguageTable *table) {
    if (!table) {
        return;
    }

    if (table->entries) {
        for (size_t i = 0; i < table->count; i++) {
            free(table->entries[i].code);
            free(table->entries[i].name);
        }
        free(table->entries);
    }

    if (table->mappings) {
        for (size_t i = 0; i < table->mapping_count; i++) {
            free(table->mappings[i].iso639_2);
            free(table->mappings[i].iso639_1);
        }
        free(table->mappings);
    }

    free(table);
}

/* Look up language name by code */
const char *language_table_lookup(const LanguageTable *table, const char *code) {
    if (!table || !code || !table->entries) {
        return NULL;
    }

    /* Case-insensitive search */
    for (size_t i = 0; i < table->count; i++) {
        if (strcasecmp(table->entries[i].code, code) == 0) {
            return table->entries[i].name;
        }
    }

    return NULL;
}

/* Reverse lookup: find code by language name */
const char *language_table_reverse_lookup(const LanguageTable *table, const char *name) {
    if (!table || !name || !table->entries) {
        return NULL;
    }

    /* Case-insensitive search */
    for (size_t i = 0; i < table->count; i++) {
        if (strcasecmp(table->entries[i].name, name) == 0) {
            return table->entries[i].code;
        }
    }

    return NULL;
}

/* Check if code exists */
bool language_table_has_code(const LanguageTable *table, const char *code) {
    return language_table_lookup(table, code) != NULL;
}

/* Get base language code (e.g., "en-US" -> "en") */
int language_table_get_base_code(const char *code, char *base_code, size_t buf_size) {
    if (!code || !base_code || buf_size == 0) {
        return -1;
    }

    /* Find first hyphen */
    const char *hyphen = strchr(code, '-');

    if (hyphen) {
        size_t base_len = hyphen - code;
        if (base_len >= buf_size) {
            base_len = buf_size - 1;
        }
        strncpy(base_code, code, base_len);
        base_code[base_len] = '\0';
    } else {
        /* No hyphen, copy entire code */
        strncpy(base_code, code, buf_size - 1);
        base_code[buf_size - 1] = '\0';
    }

    return 0;
}

/* Get entry count */
size_t language_table_count(const LanguageTable *table) {
    if (!table) {
        return 0;
    }
    return table->count;
}

/* Convert ISO 639-2 to ISO 639-1 */
const char *language_table_to_iso639_1(const LanguageTable *table, const char *iso639_2) {
    if (!table || !iso639_2 || !table->mappings) {
        return NULL;
    }

    /* Case-insensitive search */
    for (size_t i = 0; i < table->mapping_count; i++) {
        if (strcasecmp(table->mappings[i].iso639_2, iso639_2) == 0) {
            return table->mappings[i].iso639_1;
        }
    }

    return NULL;
}

/* Convert ISO 639-1 to ISO 639-2 */
const char *language_table_to_iso639_2(const LanguageTable *table, const char *iso639_1) {
    if (!table || !iso639_1 || !table->mappings) {
        return NULL;
    }

    /* Case-insensitive search */
    for (size_t i = 0; i < table->mapping_count; i++) {
        if (strcasecmp(table->mappings[i].iso639_1, iso639_1) == 0) {
            return table->mappings[i].iso639_2;
        }
    }

    return NULL;
}

/* Resolve language information from any code format */
int language_table_resolve(const LanguageTable *table, const char *code,
                          const char **out_name,
                          const char **out_iso639_1,
                          const char **out_iso639_2) {
    if (!table || !code) {
        return -1;
    }

    const char *name = NULL;
    const char *iso639_1 = NULL;
    const char *iso639_2 = NULL;

    size_t code_len = strlen(code);

    if (code_len == 3) {
        /* Input is 3-letter code (ISO 639-2) */
        iso639_2 = code;
        iso639_1 = language_table_to_iso639_1(table, code);

        /* Look up name using either code */
        name = language_table_lookup(table, code);
        if (!name && iso639_1) {
            name = language_table_lookup(table, iso639_1);
        }
    } else if (code_len == 2) {
        /* Input is 2-letter code (ISO 639-1) */
        iso639_1 = code;
        iso639_2 = language_table_to_iso639_2(table, code);

        /* Look up name */
        name = language_table_lookup(table, code);
    } else {
        /* Try regional variant (e.g., "en-US", "zh-Hans") */
        name = language_table_lookup(table, code);

        if (name) {
            /* Extract base code */
            char base_code[8];
            if (language_table_get_base_code(code, base_code, sizeof(base_code)) == 0) {
                size_t base_len = strlen(base_code);
                if (base_len == 2) {
                    iso639_1 = NULL;  /* Will need to find in entries */
                    iso639_2 = language_table_to_iso639_2(table, base_code);

                    /* Find the actual stored iso639_1 */
                    for (size_t i = 0; i < table->mapping_count; i++) {
                        if (strcasecmp(table->mappings[i].iso639_1, base_code) == 0) {
                            iso639_1 = table->mappings[i].iso639_1;
                            break;
                        }
                    }
                } else if (base_len == 3) {
                    iso639_2 = NULL;
                    iso639_1 = language_table_to_iso639_1(table, base_code);

                    for (size_t i = 0; i < table->mapping_count; i++) {
                        if (strcasecmp(table->mappings[i].iso639_2, base_code) == 0) {
                            iso639_2 = table->mappings[i].iso639_2;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (!name) {
        return -1;
    }

    /* Set output values if pointers provided */
    if (out_name) *out_name = name;
    if (out_iso639_1) *out_iso639_1 = iso639_1;
    if (out_iso639_2) *out_iso639_2 = iso639_2;

    return 0;
}
