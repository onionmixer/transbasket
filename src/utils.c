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
#include "utils.h"

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

/* Normalize language code or name to ISO 639-2 code */
const char *normalize_language_code(const char *lang_input) {
    if (!lang_input) {
        return NULL;
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
    struct tm *tm_utc = gmtime(&now);

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
