/**
 * Utility functions for transbasket.
 * Common utilities for validation, formatting, and helper functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <uuid/uuid.h>
#include <regex.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include "utils.h"

/* Get current timestamp for logging in [YYYY-MM-DD HH:MM:SS] format */
void get_log_timestamp(char *buffer, size_t size) {
    if (!buffer || size < 21) {
        return;
    }

    time_t now = time(NULL);
    struct tm local_storage;
    struct tm *tm_local = localtime_r(&now, &local_storage);

    if (!tm_local) {
        snprintf(buffer, size, "[INVALID TIMESTAMP]");
        return;
    }

    snprintf(buffer, size, "[%04d-%02d-%02d %02d:%02d:%02d]",
             tm_local->tm_year + 1900,
             tm_local->tm_mon + 1,
             tm_local->tm_mday,
             tm_local->tm_hour,
             tm_local->tm_min,
             tm_local->tm_sec);
}

/* Log message with timestamp and level */
void log_message(const char *level, const char *format, ...) {
    char timestamp[32];
    get_log_timestamp(timestamp, sizeof(timestamp));

    fprintf(stdout, "%s[%s] ", timestamp, level);

    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);

    fprintf(stdout, "\n");
    fflush(stdout);
}

/* ISO 639-2 language codes */
static const char *ISO_639_2_CODES[] = {
    "aar", "abk", "ace", "ach", "ada", "ady", "afr", "aka", "alb", "amh",
    "ara", "arg", "arm", "asm", "ava", "ave", "aym", "aze", "bak", "bam",
    "baq", "bel", "ben", "bih", "bis", "bos", "bre", "bul", "bur", "cat",
    "ceb", "cha", "che", "chi", "chu", "chv", "cor", "cos", "cre", "cze",
    "dan", "div", "dut", "dzo", "eng", "epo", "est", "ewe", "fao", "fij",
    "fin", "fre", "fry", "ful", "geo", "ger", "gla", "gle", "glg", "glv",
    "gre", "grn", "guj", "hat", "hau", "heb", "her", "hin", "hmo", "hrv",
    "hun", "ibo", "ice", "ido", "iii", "iku", "ile", "ina", "ind", "ipk",
    "ita", "jav", "jpn", "kal", "kan", "kas", "kaz", "khm", "kik", "kin",
    "kir", "kom", "kon", "kor", "kua", "kur", "lao", "lat", "lav", "lim",
    "lin", "lit", "ltz", "lub", "lug", "mac", "mah", "mal", "mao", "mar",
    "may", "mlg", "mlt", "mon", "nau", "nav", "nbl", "nde", "ndo", "nep",
    "nno", "nob", "nor", "nya", "oci", "oji", "ori", "orm", "oss", "pan",
    "per", "pli", "pol", "por", "pus", "que", "roh", "rum", "run", "rus",
    "sag", "san", "sin", "slo", "slv", "sme", "smo", "sna", "snd", "som",
    "sot", "spa", "srd", "srp", "ssw", "sun", "swa", "swe", "tah", "tam",
    "tat", "tel", "tgk", "tgl", "tha", "tib", "tir", "ton", "tsn", "tso",
    "tuk", "tur", "twi", "uig", "ukr", "urd", "uzb", "ven", "vie", "vol",
    "wel", "wln", "wol", "xho", "yid", "yor", "zha", "zul",
    /* transbasket 전용 3글자 코드 (ISO 639-2 미할당 영역).
     * 중국어 스크립트를 구분하기 위해 도입했다. 캐시의 3글자 제약을 지키면서
     * zh-Hans / zh-Hant 를 서로 다른 캐시 키로 분리한다.
     * 자세한 내용은 PLAN_CHINESE_SCRIPT_CODES.md 참조. */
    "zhs",  /* Chinese (Simplified)  <- zh-Hans, zh-CN, zh-SG, zh-MY */
    "zht",  /* Chinese (Traditional) <- zh-Hant, zh-TW, zh-HK, zh-MO */
    NULL
};

/* Language code to name mapping */
typedef struct {
    const char *code;
    const char *name;
} LanguageMap;

static const LanguageMap LANGUAGE_NAMES[] = {
    {"eng", "English"},
    {"kor", "Korean"},
    {"jpn", "Japanese"},
    {"chi", "Chinese"},
    {"spa", "Spanish"},
    {"fre", "French"},
    {"ger", "German"},
    {"rus", "Russian"},
    {"ara", "Arabic"},
    {"por", "Portuguese"},
    {"ita", "Italian"},
    {"dut", "Dutch"},
    {"pol", "Polish"},
    {"tur", "Turkish"},
    {"vie", "Vietnamese"},
    {"tha", "Thai"},
    {"ind", "Indonesian"},
    {"may", "Malay"},
    {"hin", "Hindi"},
    {"ben", "Bengali"},
    /* transbasket 전용 코드. 이름이 없으면 프롬프트 언어명이 비거나 코드가 그대로
     * 들어가므로 반드시 함께 유지할 것. */
    {"zhs", "Chinese (Simplified)"},
    {"zht", "Chinese (Traditional)"},
    {NULL, NULL}
};

/* Validate ISO 639-2 language code */
bool validate_language_code(const char *lang_code) {
    if (!lang_code || strlen(lang_code) != 3) {
        return false;
    }

    char lower_code[4];
    for (int i = 0; i < 3; i++) {
        lower_code[i] = tolower(lang_code[i]);
    }
    lower_code[3] = '\0';

    for (int i = 0; ISO_639_2_CODES[i] != NULL; i++) {
        if (strcmp(lower_code, ISO_639_2_CODES[i]) == 0) {
            return true;
        }
    }

    return false;
}

/* Validate UUID v4 format */
bool validate_uuid(const char *uuid_str) {
    if (!uuid_str || strlen(uuid_str) != 36) {
        return false;
    }

    regex_t regex;
    const char *pattern = "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$";

    if (regcomp(&regex, pattern, REG_EXTENDED | REG_ICASE) != 0) {
        return false;
    }

    int result = regexec(&regex, uuid_str, 0, NULL, 0);
    regfree(&regex);

    return result == 0;
}

/* Validate RFC 3339 timestamp format */
bool validate_timestamp(const char *timestamp_str) {
    if (!timestamp_str) {
        return false;
    }

    regex_t regex;
    const char *pattern = "^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]+)?(Z|[+-][0-9]{2}:[0-9]{2})$";

    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        return false;
    }

    int result = regexec(&regex, timestamp_str, 0, NULL, 0);
    regfree(&regex);

    return result == 0;
}

/* Get human-readable language name */
const char *get_language_name(const char *lang_code) {
    if (!lang_code || strlen(lang_code) != 3) {
        return NULL;
    }

    char lower_code[4];
    for (int i = 0; i < 3; i++) {
        lower_code[i] = tolower(lang_code[i]);
    }
    lower_code[3] = '\0';

    for (int i = 0; LANGUAGE_NAMES[i].code != NULL; i++) {
        if (strcmp(lower_code, LANGUAGE_NAMES[i].code) == 0) {
            return LANGUAGE_NAMES[i].name;
        }
    }

    return NULL;
}

/* ---- Chinese script tag handling -------------------------------------------
 *
 * The cache stores 3-character language codes (see the CHECK constraints in
 * cache_sqlite_schema.sql), but clients send BCP-47 tags such as "zh-Hans".
 * Those tags are folded here into the transbasket-specific codes "zhs"
 * (Simplified) and "zht" (Traditional) so the rest of the server - buffers,
 * cache keys, schema - keeps working unchanged. See
 * PLAN_CHINESE_SCRIPT_CODES.md for the full rationale.
 * -------------------------------------------------------------------------- */

#define ZH_SIMPLIFIED   "zhs"
#define ZH_TRADITIONAL  "zht"
#define LANG_TAG_MAX    32   /* longest tag we fold; longer input is left alone */
#define LANG_SUBTAG_MAX 8    /* BCP-47 subtag length limit */
#define LANG_SUBTAG_COUNT 8  /* how many subtags we are willing to look at */

typedef struct {
    const char *subtag;  /* lowercased BCP-47 subtag */
    const char *code;    /* transbasket 3-letter code it maps to */
} ChineseSubtagMap;

/* Script subtags decide the result before any region subtag does. */
static const ChineseSubtagMap ZH_SCRIPT_SUBTAGS[] = {
    {"hans", ZH_SIMPLIFIED},
    {"hant", ZH_TRADITIONAL},
    {NULL, NULL}
};

/* Region subtags are only consulted when no script subtag is present. */
static const ChineseSubtagMap ZH_REGION_SUBTAGS[] = {
    {"cn", ZH_SIMPLIFIED},  {"sg", ZH_SIMPLIFIED},  {"my", ZH_SIMPLIFIED},
    {"tw", ZH_TRADITIONAL}, {"hk", ZH_TRADITIONAL}, {"mo", ZH_TRADITIONAL},
    {NULL, NULL}
};

/* Lowercase `in` into `buf`, mapping '_' to '-' so locale-style tags such as
 * "zh_Hans" are accepted. Returns -1 when the input does not fit, which keeps
 * oversized input away from the fixed buffer. */
static int fold_lang_tag(const char *in, char *buf, size_t buf_size) {
    if (!in || !buf || buf_size == 0) {
        return -1;
    }

    size_t len = strlen(in);
    if (len == 0 || len >= buf_size) {
        return -1;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)in[i];  /* tolower() needs unsigned */
        buf[i] = (ch == '_') ? '-' : (char)tolower(ch);
    }
    buf[len] = '\0';

    return 0;
}

/* Map a folded (lowercased) language tag to a Chinese code, or NULL when the
 * tag is not Chinese - in which case the caller falls through to the regular
 * code/name lookup. A malformed tag also returns NULL and ends up rejected.
 *
 * Rules (PLAN_CHINESE_SCRIPT_CODES.md 4):
 *   - every subtag must be a non-empty [a-z0-9] run of at most 8 characters
 *   - "zh"/"zho" alone means Chinese with no script preference -> "chi"
 *   - a script subtag (hans/hant) wins over any region subtag
 *   - otherwise a known region subtag decides
 *   - anything else is rejected
 */
static const char *normalize_chinese_tag(const char *folded) {
    char subtags[LANG_SUBTAG_COUNT][LANG_SUBTAG_MAX + 1];
    int subtag_count = 0;

    const char *cursor = folded;
    while (*cursor) {
        const char *dash = strchr(cursor, '-');
        size_t len = dash ? (size_t)(dash - cursor) : strlen(cursor);

        if (len == 0 || len > LANG_SUBTAG_MAX) {
            return NULL;  /* empty subtag (leading/double/trailing '-') or too long */
        }
        if (subtag_count >= LANG_SUBTAG_COUNT) {
            return NULL;  /* absurd number of subtags */
        }
        for (size_t i = 0; i < len; i++) {
            unsigned char ch = (unsigned char)cursor[i];
            if (ch > 127 || !isalnum(ch)) {
                return NULL;  /* non-ASCII or punctuation inside a subtag */
            }
        }

        memcpy(subtags[subtag_count], cursor, len);
        subtags[subtag_count][len] = '\0';
        subtag_count++;

        if (!dash) {
            break;
        }
        cursor = dash + 1;
        if (*cursor == '\0') {
            return NULL;  /* trailing '-' */
        }
    }

    if (subtag_count == 0) {
        return NULL;
    }

    if (subtag_count == 1) {
        if (strcmp(subtags[0], "zh") == 0 || strcmp(subtags[0], "zho") == 0) {
            return "chi";  /* no script requested: behave exactly like "chi" */
        }
        return NULL;
    }

    if (strcmp(subtags[0], "zh") != 0) {
        return NULL;
    }

    for (int i = 1; i < subtag_count; i++) {
        for (int k = 0; ZH_SCRIPT_SUBTAGS[k].subtag != NULL; k++) {
            if (strcmp(subtags[i], ZH_SCRIPT_SUBTAGS[k].subtag) == 0) {
                return ZH_SCRIPT_SUBTAGS[k].code;
            }
        }
    }

    for (int i = 1; i < subtag_count; i++) {
        for (int k = 0; ZH_REGION_SUBTAGS[k].subtag != NULL; k++) {
            if (strcmp(subtags[i], ZH_REGION_SUBTAGS[k].subtag) == 0) {
                return ZH_REGION_SUBTAGS[k].code;
            }
        }
    }

    return NULL;  /* "zh" with only unknown subtags */
}

bool is_auto_source_language(const char *lang_input) {
    return lang_input && strcasecmp(lang_input, "auto") == 0;
}

const char *normalize_source_language(const char *lang_input) {
    if (is_auto_source_language(lang_input)) return "auto";
    return normalize_language_code(lang_input);
}

/* Normalize language code or name to a supported 3-letter code */
const char *normalize_language_code(const char *lang_input) {
    if (!lang_input) {
        return NULL;
    }

    /* Chinese tags are handled first: "zho" is three characters but is not an
     * ISO_639_2_CODES entry, so the 3-letter branch below would reject it. */
    char folded[LANG_TAG_MAX];
    if (fold_lang_tag(lang_input, folded, sizeof(folded)) == 0) {
        const char *chinese = normalize_chinese_tag(folded);
        if (chinese) {
            return chinese;
        }
    }

    size_t len = strlen(lang_input);

    /* If it's a 3-letter code, validate and return if valid */
    if (len == 3) {
        if (validate_language_code(lang_input)) {
            /* Return the canonical lowercase version from our list */
            char lower_code[4];
            for (int i = 0; i < 3; i++) {
                lower_code[i] = tolower(lang_input[i]);
            }
            lower_code[3] = '\0';

            for (int i = 0; ISO_639_2_CODES[i] != NULL; i++) {
                if (strcmp(lower_code, ISO_639_2_CODES[i]) == 0) {
                    return ISO_639_2_CODES[i];
                }
            }
        }
        return NULL;
    }

    /* Try to match as language name (case-insensitive) */
    for (int i = 0; LANGUAGE_NAMES[i].code != NULL; i++) {
        if (strcasecmp(lang_input, LANGUAGE_NAMES[i].name) == 0) {
            return LANGUAGE_NAMES[i].code;
        }
    }

    return NULL;
}

/* Generate UUID v4 */
int generate_uuid(char *uuid_buf, size_t buf_size) {
    if (!uuid_buf || buf_size < 37) {
        return -1;
    }

    uuid_t uuid;
    uuid_generate_random(uuid);
    uuid_unparse_lower(uuid, uuid_buf);

    return 0;
}

/* Get current timestamp in RFC 3339 format */
int get_current_timestamp(char *timestamp_buf, size_t buf_size) {
    if (!timestamp_buf || buf_size < 64) {
        return -1;
    }

    time_t now = time(NULL);
    struct tm utc_storage;
    struct tm *tm_utc = gmtime_r(&now, &utc_storage);

    if (!tm_utc) {
        return -1;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int milliseconds = ts.tv_nsec / 1000000;

    snprintf(timestamp_buf, buf_size,
             "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm_utc->tm_year + 1900,
             tm_utc->tm_mon + 1,
             tm_utc->tm_mday,
             tm_utc->tm_hour,
             tm_utc->tm_min,
             tm_utc->tm_sec,
             milliseconds);

    return 0;
}

/* Forward declaration for UTF-8 decoding helper */
static int utf8_decode(const unsigned char *s, unsigned int *cp);

/* Truncate text with suffix (UTF-8 aware) */
int truncate_text(const char *text, char *output, size_t max_length, const char *suffix) {
    if (!text || !output || max_length == 0) {
        return -1;
    }

    if (!suffix) {
        suffix = "...";
    }

    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);

    if (text_len <= max_length) {
        strncpy(output, text, max_length);
        output[max_length] = '\0';
        return 0;
    }

    if (suffix_len >= max_length) {
        strncpy(output, suffix, max_length);
        output[max_length] = '\0';
        return 0;
    }

    /* Calculate max bytes for text (leaving room for suffix) */
    size_t max_text_bytes = max_length - suffix_len;

    /* Find the last complete UTF-8 character within max_text_bytes */
    size_t byte_pos = 0;
    size_t last_complete_pos = 0;

    while (byte_pos < text_len && byte_pos < max_text_bytes) {
        unsigned int codepoint;
        int char_len = utf8_decode((unsigned char *)&text[byte_pos], &codepoint);

        /* Check if this character would exceed the limit */
        if (byte_pos + char_len > max_text_bytes) {
            break;
        }

        byte_pos += char_len;
        last_complete_pos = byte_pos;
    }

    /* Copy only complete UTF-8 characters */
    memcpy(output, text, last_complete_pos);
    strcpy(output + last_complete_pos, suffix);

    return 0;
}

/* Helper function to check if character is emoji or part of emoji range */
static bool is_emoji_codepoint(unsigned int cp) {
    return (cp >= 0x1F300 && cp <= 0x1F5FF) ||  // Symbols & Pictographs
           (cp >= 0x1F600 && cp <= 0x1F64F) ||  // Emoticons
           (cp >= 0x1F680 && cp <= 0x1F6FF) ||  // Transport & Map
           (cp >= 0x1F700 && cp <= 0x1F77F) ||  // Alchemical
           (cp >= 0x1F780 && cp <= 0x1F7FF) ||  // Geometric
           (cp >= 0x1F800 && cp <= 0x1F8FF) ||  // Supplemental Arrows-C
           (cp >= 0x1F900 && cp <= 0x1F9FF) ||  // Supplemental Symbols
           (cp >= 0x1FA00 && cp <= 0x1FA6F) ||  // Chess etc.
           (cp >= 0x1FA70 && cp <= 0x1FAFF) ||  // Extended-A
           (cp >= 0x2700 && cp <= 0x27BF) ||    // Dingbats
           (cp >= 0x2600 && cp <= 0x26FF);      // Misc Symbols
}

/* Decode UTF-8 character and return codepoint */
static int utf8_decode(const unsigned char *s, unsigned int *cp) {
    if ((s[0] & 0x80) == 0) {
        *cp = s[0];
        return 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        *cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        *cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    } else if ((s[0] & 0xF8) == 0xF0) {
        *cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    return 1;
}

/* Strip emoji and shortcodes from text */
int strip_emoji_and_shortcodes(const char *input, char *output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return -1;
    }

    size_t in_len = strlen(input);
    size_t out_pos = 0;
    size_t in_pos = 0;
    bool in_shortcode = false;
    bool last_was_space = false;

    while (in_pos < in_len && out_pos < output_size - 1) {
        unsigned int codepoint;
        int char_len = utf8_decode((unsigned char *)&input[in_pos], &codepoint);

        // Check for shortcode start
        if (input[in_pos] == ':' && !in_shortcode) {
            in_shortcode = true;
            in_pos++;
            continue;
        }

        // Check for shortcode end
        if (in_shortcode) {
            if (input[in_pos] == ':') {
                in_shortcode = false;
                in_pos++;
                continue;
            } else if (!isalnum(input[in_pos]) && input[in_pos] != '_' &&
                       input[in_pos] != '+' && input[in_pos] != '-' && input[in_pos] != '&') {
                // Not a valid shortcode, output the stored ':'
                in_shortcode = false;
            } else {
                in_pos++;
                continue;
            }
        }

        // Check for emoji
        if (is_emoji_codepoint(codepoint)) {
            in_pos += char_len;
            continue;
        }

        // Handle whitespace normalization
        if (isspace(input[in_pos])) {
            // Preserve newlines
            if (input[in_pos] == '\n') {
                if (out_pos < output_size - 1) {
                    output[out_pos++] = '\n';
                }
                last_was_space = false;  // Reset for next line
                in_pos++;
                continue;
            }

            // Normalize other whitespace to single space
            if (!last_was_space && out_pos > 0 && out_pos < output_size - 1) {
                output[out_pos++] = ' ';
                last_was_space = true;
            }
            in_pos++;
            continue;
        }

        // Copy character
        last_was_space = false;
        for (int i = 0; i < char_len && in_pos < in_len && out_pos < output_size - 1; i++) {
            output[out_pos++] = input[in_pos++];
        }
    }

    // Remove trailing space
    if (out_pos > 0 && output[out_pos - 1] == ' ') {
        out_pos--;
    }

    output[out_pos] = '\0';
    return 0;
}

/* Unescape string (convert \\n to \n, \\t to \t, etc.) */
int unescape_string(const char *input, char *output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return -1;
    }

    size_t in_len = strlen(input);
    size_t out_pos = 0;
    size_t in_pos = 0;

    while (in_pos < in_len && out_pos < output_size - 1) {
        if (input[in_pos] == '\\' && in_pos + 1 < in_len) {
            // Handle escape sequences
            switch (input[in_pos + 1]) {
                case 'n':
                    output[out_pos++] = '\n';
                    in_pos += 2;
                    break;
                case 't':
                    output[out_pos++] = '\t';
                    in_pos += 2;
                    break;
                case 'r':
                    output[out_pos++] = '\r';
                    in_pos += 2;
                    break;
                case '\\':
                    output[out_pos++] = '\\';
                    in_pos += 2;
                    break;
                case '"':
                    output[out_pos++] = '"';
                    in_pos += 2;
                    break;
                case '\'':
                    output[out_pos++] = '\'';
                    in_pos += 2;
                    break;
                default:
                    // Not a recognized escape sequence, copy backslash as-is
                    output[out_pos++] = input[in_pos++];
                    break;
            }
        } else {
            // Regular character
            output[out_pos++] = input[in_pos++];
        }
    }

    output[out_pos] = '\0';
    return 0;
}

/* Strip ANSI escape codes and control characters from text */
int strip_ansi_codes(const char *input, char *output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return -1;
    }

    size_t in_len = strlen(input);
    size_t out_pos = 0;
    size_t in_pos = 0;

    while (in_pos < in_len && out_pos < output_size - 1) {
        unsigned char ch = (unsigned char)input[in_pos];

        /* Handle ANSI escape sequences starting with ESC (0x1B) */
        if (ch == 0x1B && in_pos + 1 < in_len) {
            /* Check for CSI sequence: ESC[ ... letter */
            if (input[in_pos + 1] == '[') {
                in_pos += 2;  /* Skip ESC[ */

                /* Skip parameter bytes (0-9, ;, :, etc.) and intermediate bytes */
                while (in_pos < in_len) {
                    unsigned char seq_ch = (unsigned char)input[in_pos];

                    /* CSI sequence ends with a letter (0x40-0x7E) */
                    if (seq_ch >= 0x40 && seq_ch <= 0x7E) {
                        in_pos++;  /* Skip the final letter */
                        break;
                    }

                    /* Parameter bytes: 0x30-0x3F (0-9, ;, <, =, >, ?) */
                    /* Intermediate bytes: 0x20-0x2F (space, !, ", #, etc.) */
                    if ((seq_ch >= 0x20 && seq_ch <= 0x3F)) {
                        in_pos++;
                        continue;
                    }

                    /* Invalid sequence, stop */
                    break;
                }
                continue;
            }

            /* Other ESC sequences (2-byte): ESC + letter */
            if (input[in_pos + 1] >= 0x40 && input[in_pos + 1] <= 0x5F) {
                in_pos += 2;  /* Skip ESC and the letter */
                continue;
            }

            /* Unknown ESC sequence, skip ESC and continue */
            in_pos++;
            continue;
        }

        /* Strip other control characters except \n (0x0A) and \r (0x0D) */
        if (ch < 0x20) {
            if (ch == '\n' || ch == '\r') {
                /* Preserve newlines and carriage returns */
                output[out_pos++] = ch;
            }
            /* Skip other control characters (\t, \b, etc.) */
            in_pos++;
            continue;
        }

        /* Strip DEL character (0x7F) */
        if (ch == 0x7F) {
            in_pos++;
            continue;
        }

        /* Copy normal character */
        output[out_pos++] = ch;
        in_pos++;
    }

    output[out_pos] = '\0';
    return 0;
}

/* Strip control characters (0x00-0x1F) from text, except CR and LF */
int strip_control_characters(const char *input, char *output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return -1;
    }

    size_t in_len = strlen(input);
    size_t out_pos = 0;
    size_t in_pos = 0;

    while (in_pos < in_len && out_pos < output_size - 1) {
        unsigned char ch = (unsigned char)input[in_pos];

        /* Filter control characters (0x00-0x1F) EXCEPT CR(0x0D), LF(0x0A) */
        /* These control characters can corrupt UTF-8 multibyte sequences */
        if (ch < 0x20 && ch != 0x0D && ch != 0x0A) {
            /* Skip this control character */
            in_pos++;
            continue;
        }

        /* Copy normal character */
        output[out_pos++] = ch;
        in_pos++;
    }

    output[out_pos] = '\0';
    return 0;
}

/* Daemonize the process */
int daemonize(void) {
    pid_t pid, sid;

    /* Fork the parent process */
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Error: fork() failed\n");
        return -1;
    }

    /* Exit the parent process */
    if (pid > 0) {
        exit(0);
    }

    /* Create a new session */
    sid = setsid();
    if (sid < 0) {
        fprintf(stderr, "Error: setsid() failed\n");
        return -1;
    }

    /* Fork again to ensure we're not a session leader */
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Error: second fork() failed\n");
        return -1;
    }

    /* Exit the first child */
    if (pid > 0) {
        exit(0);
    }

    /* Change working directory to root */
    if (chdir("/") < 0) {
        fprintf(stderr, "Error: chdir() failed\n");
        return -1;
    }

    /* Reset file mode creation mask */
    umask(0);

    /* Close standard file descriptors */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    /* Redirect standard file descriptors to /dev/null */
    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) {
        return -1;
    }

    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);

    if (fd > 2) {
        close(fd);
    }

    return 0;
}
