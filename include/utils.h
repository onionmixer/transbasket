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

/* Also append every log line to fd (-1 = off); daemon startup only (daemon.c). */
void log_set_tee(int fd);

/* Get current timestamp for logging */
void get_log_timestamp(char *buffer, size_t size);

/* atoi() for every input where atoi is defined (glibc: (int)strtol(s, NULL, 10) -
 * leading isspace, optional sign, digits, the rest ignored); a value outside the
 * int range saturates instead of being undefined. *plain is false when there are
 * no digits, characters follow them, or the value saturated. */
int atoi_saturating(const char *value, bool *plain);

/* Copy an untrusted string into out for a log line: control bytes and DEL become
 * \n, \r, \t or \xNN and a backslash becomes \\, so the text cannot start a
 * line of its own (PLAN_OPEN_ISSUES.md B). At most max_in input bytes are used;
 * "..." marks a cut. out is always terminated; no allocation. NULL -> "(null)". */
void log_escape(const char *in, size_t max_in, char *out, size_t out_size);

/* Language code validation. Accepts ISO 639-2 codes plus the transbasket
 * specific codes "zhs"/"zht" (Chinese Simplified/Traditional). */
bool validate_language_code(const char *lang_code);

/* i-th entry of the supported code list (ISO 639-2/B plus zhs/zht), NULL past the end.
 * The single source of the canonical codes, e.g. for the SQLite hint code table. */
const char *iso_639_2_code_at(size_t i);

/* UUID v4 validation (RFC 4122) */
bool validate_uuid(const char *uuid_str);

/* Longest accepted request timestamp in bytes; TranslationRequest.timestamp holds
 * it plus the NUL, so an accepted value is never truncated (PLAN_CODE_ISSUES_4.md 2). */
#define TIMESTAMP_MAX_LEN 63

/* RFC 3339 date-time (section 5.6) with range checks: month 01-12, day within the
 * month (Gregorian leap years, 0000 included), hour 00-23, minute 00-59, second
 * 00-60 (60 accepted at any time - looser than section 5.7, which ties it to real
 * leap seconds), optional "." and 1+ digits, then "Z" or +/-HH:MM (00-23, 00-59).
 * Upper-case "T" and "Z" only, at most TIMESTAMP_MAX_LEN bytes. Specification:
 * tests/timestamp_ref.py. */
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

#endif /* UTILS_H */
