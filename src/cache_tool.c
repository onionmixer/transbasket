/**
 * Cache management tool for transbasket.
 * Provides utilities to manage translation cache database.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "trans_cache.h"
#include "utils.h"

#define VERSION "1.0.0"
#define DEFAULT_CACHE_FILE "trans_dictionary.txt"

/* Print usage information */
static void print_usage(const char *prog_name) {
    printf("Transbasket Cache Management Tool v%s\n\n", VERSION);
    printf("Usage: %s <command> [options]\n\n", prog_name);
    printf("Commands:\n");
    printf("  list [from_lang] [to_lang]       List cache entries\n");
    printf("                                    Optional: filter by language pair\n");
    printf("  clear <from_lang> <to_lang>      Clear cache entries for language pair\n");
    printf("  clear-all                        Clear all cache entries\n");
    printf("  stats                            Show cache statistics\n");
    printf("  cleanup <days>                   Remove entries older than <days>\n");
    printf("  search <from_lang> <to_lang> <text>\n");
    printf("                                   Search for specific translation\n");
    printf("  delete <id>                      Delete entry by ID\n");
    printf("  export [from_lang] [to_lang]     Export cache entries to stdout\n");
    printf("                                    Optional: filter by language pair\n");
    printf("\n");
    printf("Options:\n");
    printf("  -f <file>                        Specify cache file (default: %s)\n", DEFAULT_CACHE_FILE);
    printf("  -h, --help                       Show this help message\n");
    printf("  -v, --version                    Show version information\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s list                          List all cache entries\n", prog_name);
    printf("  %s list kor eng                  List Korean to English entries\n", prog_name);
    printf("  %s clear kor eng                 Clear Korean to English cache\n", prog_name);
    printf("  %s cleanup 30                    Remove entries older than 30 days\n", prog_name);
    printf("  %s stats                         Show cache statistics\n", prog_name);
    printf("  %s -f custom.txt list            Use custom cache file\n", prog_name);
    printf("\n");
}

/* Print version information */
static void print_version(void) {
    printf("Transbasket Cache Tool v%s\n", VERSION);
}

/* Format timestamp to readable string */
static void format_timestamp(time_t timestamp, char *buffer, size_t size) {
    struct tm *tm_info = localtime(&timestamp);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

/* Truncate text for display */
static void truncate_for_display(const char *text, char *buffer, size_t max_len) {
    size_t len = strlen(text);
    if (len <= max_len) {
        strncpy(buffer, text, max_len + 1);
    } else {
        snprintf(buffer, max_len + 1, "%.*s...", (int)(max_len - 3), text);
    }
}

/* List cache entries */
static int cmd_list(TransCache *cache, const char *from_lang, const char *to_lang) {
    if (!cache) {
        return -1;
    }

    printf("\n");
    printf("%-5s %-4s %-4s %-8s %-30s %-30s %-19s\n",
           "ID", "From", "To", "Count", "Source", "Translation", "Last Used");
    printf("─────────────────────────────────────────────────────────────────────────────────────────────────────────────\n");

    int displayed_count = 0;

    for (size_t i = 0; i < cache->size; i++) {
        CacheEntry *entry = cache->entries[i];

        /* Filter by language pair if specified */
        if (from_lang && strcmp(entry->from_lang, from_lang) != 0) {
            continue;
        }
        if (to_lang && strcmp(entry->to_lang, to_lang) != 0) {
            continue;
        }

        char source_display[31];
        char trans_display[31];
        char last_used_str[20];

        truncate_for_display(entry->source_text, source_display, 30);
        truncate_for_display(entry->translated_text, trans_display, 30);
        format_timestamp(entry->last_used, last_used_str, sizeof(last_used_str));

        printf("%-5d %-4s %-4s %-8d %-30s %-30s %s\n",
               entry->id, entry->from_lang, entry->to_lang, entry->count,
               source_display, trans_display, last_used_str);

        displayed_count++;
    }

    printf("\nTotal: %d entries\n\n", displayed_count);
    return 0;
}

/* Clear cache entries by language pair */
static int cmd_clear(TransCache *cache, const char *from_lang, const char *to_lang) {
    if (!cache || !from_lang || !to_lang) {
        fprintf(stderr, "Error: Missing language parameters\n");
        return -1;
    }

    /* Validate language codes */
    if (!validate_language_code(from_lang) || !validate_language_code(to_lang)) {
        fprintf(stderr, "Error: Invalid language code (must be ISO 639-2)\n");
        return -1;
    }

    int removed_count = 0;
    size_t write_idx = 0;

    for (size_t i = 0; i < cache->size; i++) {
        CacheEntry *entry = cache->entries[i];

        if (strcmp(entry->from_lang, from_lang) == 0 &&
            strcmp(entry->to_lang, to_lang) == 0) {
            /* Remove this entry */
            free(entry->source_text);
            free(entry->translated_text);
            free(entry);
            removed_count++;
        } else {
            /* Keep this entry */
            cache->entries[write_idx++] = entry;
        }
    }

    cache->size = write_idx;

    printf("Removed %d entries (%s -> %s)\n", removed_count, from_lang, to_lang);

    /* Save changes */
    if (trans_cache_save(cache) == 0) {
        printf("Cache saved successfully\n");
        return 0;
    } else {
        fprintf(stderr, "Error: Failed to save cache\n");
        return -1;
    }
}

/* Clear all cache entries */
static int cmd_clear_all(TransCache *cache) {
    if (!cache) {
        return -1;
    }

    printf("WARNING: This will delete ALL cache entries!\n");
    printf("Are you sure? (yes/no): ");

    char response[10];
    if (fgets(response, sizeof(response), stdin) == NULL) {
        fprintf(stderr, "Error: Failed to read input\n");
        return -1;
    }

    /* Remove trailing newline */
    response[strcspn(response, "\n")] = '\0';

    if (strcmp(response, "yes") != 0) {
        printf("Operation cancelled\n");
        return 0;
    }

    int total_count = cache->size;

    /* Free all entries */
    for (size_t i = 0; i < cache->size; i++) {
        CacheEntry *entry = cache->entries[i];
        free(entry->source_text);
        free(entry->translated_text);
        free(entry);
    }

    cache->size = 0;

    printf("Removed %d entries\n", total_count);

    /* Save empty cache */
    if (trans_cache_save(cache) == 0) {
        printf("Cache cleared and saved successfully\n");
        return 0;
    } else {
        fprintf(stderr, "Error: Failed to save cache\n");
        return -1;
    }
}

/* Show cache statistics */
static int cmd_stats(TransCache *cache) {
    if (!cache) {
        return -1;
    }

    /* Count entries by language pair */
    typedef struct {
        char from_lang[4];
        char to_lang[4];
        int count;
        time_t last_used;
    } LangPairStats;

    LangPairStats *pairs = NULL;
    int pairs_count = 0;
    int pairs_capacity = 10;

    pairs = malloc(pairs_capacity * sizeof(LangPairStats));
    if (!pairs) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        return -1;
    }

    /* Calculate statistics */
    time_t now = time(NULL);
    time_t oldest_time = now;
    time_t newest_time = 0;
    int total_count = cache->size;
    long total_usage_count = 0;

    for (size_t i = 0; i < cache->size; i++) {
        CacheEntry *entry = cache->entries[i];

        total_usage_count += entry->count;

        if (entry->last_used < oldest_time) {
            oldest_time = entry->last_used;
        }
        if (entry->last_used > newest_time) {
            newest_time = entry->last_used;
        }

        /* Update language pair stats */
        int found = 0;
        for (int j = 0; j < pairs_count; j++) {
            if (strcmp(pairs[j].from_lang, entry->from_lang) == 0 &&
                strcmp(pairs[j].to_lang, entry->to_lang) == 0) {
                pairs[j].count++;
                if (entry->last_used > pairs[j].last_used) {
                    pairs[j].last_used = entry->last_used;
                }
                found = 1;
                break;
            }
        }

        if (!found) {
            /* Add new pair */
            if (pairs_count >= pairs_capacity) {
                pairs_capacity *= 2;
                LangPairStats *new_pairs = realloc(pairs, pairs_capacity * sizeof(LangPairStats));
                if (!new_pairs) {
                    free(pairs);
                    fprintf(stderr, "Error: Memory reallocation failed\n");
                    return -1;
                }
                pairs = new_pairs;
            }

            strncpy(pairs[pairs_count].from_lang, entry->from_lang, 4);
            strncpy(pairs[pairs_count].to_lang, entry->to_lang, 4);
            pairs[pairs_count].count = 1;
            pairs[pairs_count].last_used = entry->last_used;
            pairs_count++;
        }
    }

    /* Print statistics */
    printf("\n");
    printf("=== Cache Statistics ===\n");
    printf("\n");
    printf("Total entries: %d\n", total_count);
    printf("Total usage count: %ld\n", total_usage_count);
    if (total_count > 0) {
        printf("Average usage per entry: %.2f\n", (double)total_usage_count / total_count);
    }
    printf("\n");

    if (total_count > 0) {
        char oldest_str[20], newest_str[20];
        format_timestamp(oldest_time, oldest_str, sizeof(oldest_str));
        format_timestamp(newest_time, newest_str, sizeof(newest_str));

        printf("Oldest entry: %s\n", oldest_str);
        printf("Newest entry: %s\n", newest_str);
        printf("\n");
    }

    printf("Entries by language pair:\n");
    printf("  %-4s → %-4s : %-8s  %-19s\n", "From", "To", "Count", "Last Used");
    printf("  ────────────────────────────────────────────────\n");

    for (int i = 0; i < pairs_count; i++) {
        char last_used_str[20];
        format_timestamp(pairs[i].last_used, last_used_str, sizeof(last_used_str));
        printf("  %-4s → %-4s : %-8d  %s\n",
               pairs[i].from_lang, pairs[i].to_lang,
               pairs[i].count, last_used_str);
    }

    printf("\n");

    free(pairs);
    return 0;
}

/* Cleanup old entries */
static int cmd_cleanup(TransCache *cache, int days) {
    if (!cache || days <= 0) {
        fprintf(stderr, "Error: Invalid days parameter\n");
        return -1;
    }

    int removed = trans_cache_cleanup(cache, days);

    printf("Removed %d entries older than %d days\n", removed, days);

    /* Save changes */
    if (trans_cache_save(cache) == 0) {
        printf("Cache saved successfully\n");
        return 0;
    } else {
        fprintf(stderr, "Error: Failed to save cache\n");
        return -1;
    }
}

/* Search for specific translation */
static int cmd_search(TransCache *cache, const char *from_lang, const char *to_lang, const char *text) {
    if (!cache || !from_lang || !to_lang || !text) {
        fprintf(stderr, "Error: Missing search parameters\n");
        return -1;
    }

    CacheEntry *entry = trans_cache_lookup(cache, from_lang, to_lang, text);

    if (!entry) {
        printf("No matching entry found\n");
        return 0;
    }

    char created_str[20], last_used_str[20];
    format_timestamp(entry->created_at, created_str, sizeof(created_str));
    format_timestamp(entry->last_used, last_used_str, sizeof(last_used_str));

    printf("\n");
    printf("=== Cache Entry Found ===\n");
    printf("\n");
    printf("ID:           %d\n", entry->id);
    printf("Hash:         %s\n", entry->hash);
    printf("From:         %s\n", entry->from_lang);
    printf("To:           %s\n", entry->to_lang);
    printf("Source:       %s\n", entry->source_text);
    printf("Translation:  %s\n", entry->translated_text);
    printf("Count:        %d\n", entry->count);
    printf("Created:      %s\n", created_str);
    printf("Last used:    %s\n", last_used_str);
    printf("\n");

    return 0;
}

/* Delete entry by ID */
static int cmd_delete(TransCache *cache, int id) {
    if (!cache || id <= 0) {
        fprintf(stderr, "Error: Invalid ID\n");
        return -1;
    }

    int found = 0;
    size_t write_idx = 0;

    for (size_t i = 0; i < cache->size; i++) {
        CacheEntry *entry = cache->entries[i];

        if (entry->id == id) {
            /* Delete this entry */
            free(entry->source_text);
            free(entry->translated_text);
            free(entry);
            found = 1;
        } else {
            /* Keep this entry */
            cache->entries[write_idx++] = entry;
        }
    }

    if (!found) {
        fprintf(stderr, "Error: Entry with ID %d not found\n", id);
        return -1;
    }

    cache->size = write_idx;

    printf("Deleted entry ID %d\n", id);

    /* Save changes */
    if (trans_cache_save(cache) == 0) {
        printf("Cache saved successfully\n");
        return 0;
    } else {
        fprintf(stderr, "Error: Failed to save cache\n");
        return -1;
    }
}

/* Export cache entries */
static int cmd_export(TransCache *cache, const char *from_lang, const char *to_lang) {
    if (!cache) {
        return -1;
    }

    for (size_t i = 0; i < cache->size; i++) {
        CacheEntry *entry = cache->entries[i];

        /* Filter by language pair if specified */
        if (from_lang && strcmp(entry->from_lang, from_lang) != 0) {
            continue;
        }
        if (to_lang && strcmp(entry->to_lang, to_lang) != 0) {
            continue;
        }

        printf("%d\t%s\t%s\t%s\t%s\t%d\t%ld\t%ld\n",
               entry->id,
               entry->from_lang,
               entry->to_lang,
               entry->source_text,
               entry->translated_text,
               entry->count,
               (long)entry->created_at,
               (long)entry->last_used);
    }

    return 0;
}

/* Main function */
int main(int argc, char *argv[]) {
    char *cache_file = DEFAULT_CACHE_FILE;
    int opt;

    /* Parse global options */
    while ((opt = getopt(argc, argv, "f:hv")) != -1) {
        switch (opt) {
            case 'f':
                cache_file = optarg;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'v':
                print_version();
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    /* Check if command is provided */
    if (optind >= argc) {
        fprintf(stderr, "Error: No command specified\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Load cache */
    TransCache *cache = trans_cache_init(cache_file);
    if (!cache) {
        fprintf(stderr, "Error: Failed to initialize cache from %s\n", cache_file);
        return 1;
    }

    /* Parse command */
    const char *command = argv[optind];
    int result = 0;

    if (strcmp(command, "list") == 0) {
        const char *from_lang = (optind + 1 < argc) ? argv[optind + 1] : NULL;
        const char *to_lang = (optind + 2 < argc) ? argv[optind + 2] : NULL;
        result = cmd_list(cache, from_lang, to_lang);

    } else if (strcmp(command, "clear") == 0) {
        if (optind + 2 >= argc) {
            fprintf(stderr, "Error: clear command requires two language codes\n");
            fprintf(stderr, "Usage: %s clear <from_lang> <to_lang>\n", argv[0]);
            result = 1;
        } else {
            result = cmd_clear(cache, argv[optind + 1], argv[optind + 2]);
        }

    } else if (strcmp(command, "clear-all") == 0) {
        result = cmd_clear_all(cache);

    } else if (strcmp(command, "stats") == 0) {
        result = cmd_stats(cache);

    } else if (strcmp(command, "cleanup") == 0) {
        if (optind + 1 >= argc) {
            fprintf(stderr, "Error: cleanup command requires days parameter\n");
            fprintf(stderr, "Usage: %s cleanup <days>\n", argv[0]);
            result = 1;
        } else {
            int days = atoi(argv[optind + 1]);
            result = cmd_cleanup(cache, days);
        }

    } else if (strcmp(command, "search") == 0) {
        if (optind + 3 >= argc) {
            fprintf(stderr, "Error: search command requires language pair and text\n");
            fprintf(stderr, "Usage: %s search <from_lang> <to_lang> <text>\n", argv[0]);
            result = 1;
        } else {
            result = cmd_search(cache, argv[optind + 1], argv[optind + 2], argv[optind + 3]);
        }

    } else if (strcmp(command, "delete") == 0) {
        if (optind + 1 >= argc) {
            fprintf(stderr, "Error: delete command requires ID parameter\n");
            fprintf(stderr, "Usage: %s delete <id>\n", argv[0]);
            result = 1;
        } else {
            int id = atoi(argv[optind + 1]);
            result = cmd_delete(cache, id);
        }

    } else if (strcmp(command, "export") == 0) {
        const char *from_lang = (optind + 1 < argc) ? argv[optind + 1] : NULL;
        const char *to_lang = (optind + 2 < argc) ? argv[optind + 2] : NULL;
        result = cmd_export(cache, from_lang, to_lang);

    } else {
        fprintf(stderr, "Error: Unknown command '%s'\n\n", command);
        print_usage(argv[0]);
        result = 1;
    }

    /* Cleanup */
    trans_cache_free(cache);

    return result;
}
