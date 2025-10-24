#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <stddef.h>

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

/* ISO 639-2 language code validation */
bool validate_language_code(const char *lang_code);

/* UUID v4 validation (RFC 4122) */
bool validate_uuid(const char *uuid_str);

/* RFC 3339 timestamp validation */
bool validate_timestamp(const char *timestamp_str);

/* Get human-readable language name from ISO 639-2 code */
const char *get_language_name(const char *lang_code);

/* Normalize language code or name to ISO 639-2 code (accepts both "kor" and "Korean") */
const char *normalize_language_code(const char *lang_input);

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
