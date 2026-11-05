#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <stddef.h>

/* Source modes need four characters ("auto") plus the terminator. */
#define SOURCE_LANGUAGE_SIZE 5

/* Logging macros */
#ifdef DEBUG
    #define LOG_DEBUG(...) log_message("DEBUG", __VA_ARGS__)
#else
    #define LOG_DEBUG(...) do {} while(0)
#endif

#define LOG_INFO(...) log_message("INFO", __VA_ARGS__)

/* Log message with timestamp and level */
void log_message(const char *level, const char *format, ...);

/* Get current timestamp for logging */
void get_log_timestamp(char *buffer, size_t size);

/* Language code validation. Accepts ISO 639-2 codes plus the transbasket
 * specific codes "zhs"/"zht" (Chinese Simplified/Traditional). */
bool validate_language_code(const char *lang_code);

/* i-th entry of the supported code list (ISO 639-2/B plus zhs/zht), NULL past the end.
 * The single source of the canonical codes, e.g. for the SQLite hint code table. */
const char *iso_639_2_code_at(size_t i);

/* UUID v4 validation (RFC 4122) */
bool validate_uuid(const char *uuid_str);

/* RFC 3339 timestamp validation */
bool validate_timestamp(const char *timestamp_str);

/* Get human-readable language name from a supported 3-letter code */
const char *get_language_name(const char *lang_code);

/* Normalize a language code or name to a supported 3-letter code.
 * Accepts "kor", "Korean", and Chinese BCP-47 tags such as "zh-Hans"
 * (-> "zhs") / "zh-Hant" (-> "zht"). See PLAN_CHINESE_SCRIPT_CODES.md. */
const char *normalize_language_code(const char *lang_input);

/* auto is a source mode, checked before any language lookup. */
bool is_auto_source_language(const char *lang_input);
const char *normalize_source_language(const char *lang_input);

/* Generate UUID v4 */
int generate_uuid(char *uuid_buf, size_t buf_size);

/* Get current timestamp in RFC 3339 format */
int get_current_timestamp(char *timestamp_buf, size_t buf_size);

/* Truncate text with suffix */
int truncate_text(const char *text, char *output, size_t max_length, const char *suffix);

/* Strip emoji and shortcodes from text */
int strip_emoji_and_shortcodes(const char *input, char *output, size_t output_size);

/* Unescape string (convert \\n to \n, \\t to \t, etc.) */
int unescape_string(const char *input, char *output, size_t output_size);

/* Strip ANSI escape codes and control characters from text */
int strip_ansi_codes(const char *input, char *output, size_t output_size);

/* Strip control characters (0x00-0x1F) from text, except CR and LF */
int strip_control_characters(const char *input, char *output, size_t output_size);

/* Daemonize the process */
int daemonize(void);

#endif /* UTILS_H */
