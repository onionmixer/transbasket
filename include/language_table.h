/**
 * Language table loader for transbasket.
 * Loads language code to name mappings from configuration file.
 * Supports ISO 639-1 (2-letter) and ISO 639-2 (3-letter) code conversion.
 */

#ifndef LANGUAGE_TABLE_H
#define LANGUAGE_TABLE_H

#include <stdbool.h>
#include <stddef.h>

/* Language entry structure */
typedef struct {
    char *code;     /* Language code (e.g., "ko", "en-US", "zh-Hans") */
    char *name;     /* Language name (e.g., "Korean", "English", "Chinese (Simplified)") */
} LanguageEntry;

/* Code mapping entry (ISO 639-2 to ISO 639-1) */
typedef struct {
    char *iso639_2;  /* 3-letter code (e.g., "kor", "eng", "jpn") */
    char *iso639_1;  /* 2-letter code (e.g., "ko", "en", "ja") */
} CodeMapping;

/* Language table structure */
typedef struct {
    LanguageEntry *entries;  /* Array of language entries */
    size_t count;            /* Number of entries */
    size_t capacity;         /* Allocated capacity */

    /* Code mappings (ISO 639-2 <-> ISO 639-1) */
    CodeMapping *mappings;   /* Array of code mappings */
    size_t mapping_count;    /* Number of mappings */
    size_t mapping_capacity; /* Allocated capacity for mappings */
} LanguageTable;

/**
 * Load language table from configuration file.
 *
 * @param file_path Path to language_tables.conf file (NULL for default)
 * @return Pointer to LanguageTable on success, NULL on failure
 */
LanguageTable *language_table_load(const char *file_path);

/**
 * Free language table and all associated memory.
 *
 * @param table Pointer to LanguageTable to free
 */
void language_table_free(LanguageTable *table);

/**
 * Look up language name by code.
 *
 * @param table Pointer to loaded LanguageTable
 * @param code Language code to look up (e.g., "ko", "en-US")
 * @return Language name if found, NULL otherwise
 */
const char *language_table_lookup(const LanguageTable *table, const char *code);

/**
 * Look up language code by name (reverse lookup).
 *
 * @param table Pointer to loaded LanguageTable
 * @param name Language name to look up (e.g., "Korean", "English")
 * @return First matching language code if found, NULL otherwise
 */
const char *language_table_reverse_lookup(const LanguageTable *table, const char *name);

/**
 * Check if a language code exists in the table.
 *
 * @param table Pointer to loaded LanguageTable
 * @param code Language code to check
 * @return true if code exists, false otherwise
 */
bool language_table_has_code(const LanguageTable *table, const char *code);

/**
 * Get base language code from a regional variant.
 * For example: "en-US" -> "en", "zh-Hans-HK" -> "zh"
 *
 * @param code Full language code
 * @param base_code Output buffer for base code
 * @param buf_size Size of output buffer
 * @return 0 on success, -1 on failure
 */
int language_table_get_base_code(const char *code, char *base_code, size_t buf_size);

/**
 * Get the number of entries in the table.
 *
 * @param table Pointer to loaded LanguageTable
 * @return Number of entries
 */
size_t language_table_count(const LanguageTable *table);

/**
 * Convert ISO 639-2 (3-letter) code to ISO 639-1 (2-letter) code.
 *
 * @param table Pointer to loaded LanguageTable
 * @param iso639_2 3-letter code (e.g., "kor", "eng")
 * @return 2-letter code (e.g., "ko", "en") or NULL if not found
 */
const char *language_table_to_iso639_1(const LanguageTable *table, const char *iso639_2);

/**
 * Convert ISO 639-1 (2-letter) code to ISO 639-2 (3-letter) code.
 *
 * @param table Pointer to loaded LanguageTable
 * @param iso639_1 2-letter code (e.g., "ko", "en")
 * @return 3-letter code (e.g., "kor", "eng") or NULL if not found
 */
const char *language_table_to_iso639_2(const LanguageTable *table, const char *iso639_1);

/**
 * Resolve language information from any code format.
 * Accepts both 2-letter and 3-letter codes.
 *
 * @param table Pointer to loaded LanguageTable
 * @param code Language code (2-letter or 3-letter)
 * @param out_name Output: language name (e.g., "Korean")
 * @param out_iso639_1 Output: ISO 639-1 code (e.g., "ko")
 * @param out_iso639_2 Output: ISO 639-2 code (e.g., "kor")
 * @return 0 on success, -1 on failure
 *
 * Note: Output pointers can be NULL if that value is not needed.
 *       The returned strings are owned by the table and should not be freed.
 */
int language_table_resolve(const LanguageTable *table, const char *code,
                          const char **out_name,
                          const char **out_iso639_1,
                          const char **out_iso639_2);

#endif /* LANGUAGE_TABLE_H */
