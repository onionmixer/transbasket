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
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include "utils.h"

void log_escape(const char *in, size_t max_in, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    if (!in) {
        snprintf(out, out_size, "(null)");
        return;
    }
    size_t o = 0;
    bool cut = false;
    for (size_t i = 0; in[i]; i++) {
        if (i >= max_in) {
            cut = true;
            break;
        }
        unsigned char c = (unsigned char)in[i];
        char tmp[5];
        size_t n;
        if (c == '\n') { memcpy(tmp, "\\n", 2); n = 2; }
        else if (c == '\r') { memcpy(tmp, "\\r", 2); n = 2; }
        else if (c == '\t') { memcpy(tmp, "\\t", 2); n = 2; }
        else if (c == '\\') { memcpy(tmp, "\\\\", 2); n = 2; }
        else if (c < 0x20 || c == 0x7F) { snprintf(tmp, sizeof(tmp), "\\x%02x", c); n = 4; }
        else { tmp[0] = (char)c; n = 1; }
        if (o + n + 4 > out_size) {   /* keep room for "..." and the terminator */
            cut = true;
            break;
        }
        memcpy(out + o, tmp, n);
        o += n;
    }
    if (cut && o + 4 <= out_size) {
        memcpy(out + o, "...", 3);
        o += 3;
    }
    out[o < out_size ? o : out_size - 1] = '\0';
}

int atoi_saturating(const char *value, bool *plain) {
    char *end = NULL;
    errno = 0;
    long n = strtol(value, &end, 10);
    bool saturated = errno == ERANGE || n > INT_MAX || n < INT_MIN;
    if (n > INT_MAX) n = INT_MAX;
    if (n < INT_MIN) n = INT_MIN;
    *plain = end != value && *end == '\0' && !saturated;
    return (int)n;
}

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
/* Daemon startup: every log line is also appended here (daemon.c), -1 = off.
 * Atomic because request threads already log while daemon_ready() turns it off;
 * daemon.c never closes the fd, so a late write still lands in the log file. */
static atomic_int g_log_tee_fd = -1;

void log_set_tee(int fd) {
    atomic_store(&g_log_tee_fd, fd);
}

/* The whole line is formatted first and written in one call, to stdout and to the
 * tee file if one is set. */
void log_message(const char *level, const char *format, ...) {
    char timestamp[32];
    get_log_timestamp(timestamp, sizeof(timestamp));

    char stack[4096];
    char *line = stack;
    int head = snprintf(stack, sizeof(stack), "%s[%s] ", timestamp, level);
    if (head < 0) {
        return;
    }
    va_list args;
    va_start(args, format);
    va_list again;
    va_copy(again, args);
    int body = vsnprintf(stack + head, sizeof(stack) - (size_t)head, format, args);
    va_end(args);
    if (body < 0) {
        va_end(again);
        return;
    }
    size_t len = (size_t)head + (size_t)body;
    if (len + 2 > sizeof(stack)) {
        line = malloc(len + 2);
        if (line) {
            memcpy(line, stack, (size_t)head);
            vsnprintf(line + head, (size_t)body + 1, format, again);
        } else {
            line = stack;   /* keep what fits */
            len = sizeof(stack) - 2;
        }
    }
    va_end(again);
    line[len] = '\n';
    line[len + 1] = '\0';

    fputs(line, stdout);
    fflush(stdout);
    int tee = atomic_load(&g_log_tee_fd);
    if (tee >= 0) {
        ssize_t w = write(tee, line, len + 1);
        (void)w;
    }
    if (line != stack) {
        free(line);
    }
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
const char *iso_639_2_code_at(size_t i) {
    for (size_t k = 0; ISO_639_2_CODES[k] != NULL; k++) {
        if (k == i) return ISO_639_2_CODES[k];
    }
    return NULL;
}

bool validate_language_code(const char *lang_code) {
    if (!lang_code || strlen(lang_code) != 3) {
        return false;
    }

    char lower_code[4];
    for (int i = 0; i < 3; i++) {
        lower_code[i] = (char)tolower((unsigned char)lang_code[i]);
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

/* n ASCII digits at s as a number, or -1 if any of them is not '0'-'9'. */
static int ts_digits(const char *s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* Validate an RFC 3339 timestamp (see utils.h for the exact rules). */
bool validate_timestamp(const char *timestamp_str) {
    if (!timestamp_str) {
        return false;
    }
    const char *s = timestamp_str;
    size_t len = strnlen(s, TIMESTAMP_MAX_LEN + 1);
    if (len > TIMESTAMP_MAX_LEN || len < 20) {   /* shortest: YYYY-MM-DDTHH:MM:SSZ */
        return false;
    }
    /* YYYY-MM-DDTHH:MM:SS: fixed positions, so a short string fails on its NUL */
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') {
        return false;
    }
    int year = ts_digits(s, 4), month = ts_digits(s + 5, 2), day = ts_digits(s + 8, 2);
    int hour = ts_digits(s + 11, 2), minute = ts_digits(s + 14, 2), second = ts_digits(s + 17, 2);
    if (year < 0 || month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 60) {
        return false;
    }
    static const int days_in_month[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    int last_day = days_in_month[month - 1] + (month == 2 && leap ? 1 : 0);
    if (day > last_day) {
        return false;
    }
    size_t i = 19;
    if (s[i] == '.') {                              /* time-secfrac: "." 1*DIGIT */
        size_t start = ++i;
        while (s[i] >= '0' && s[i] <= '9') i++;
        if (i == start) {
            return false;
        }
    }
    if (s[i] == 'Z') {
        return s[i + 1] == '\0';
    }
    if (s[i] != '+' && s[i] != '-') {
        return false;
    }
    /* +HH:MM needs 6 bytes; len bounds the reads, so s[i+1..i+5] are in the string */
    if (len - i != 6 || s[i + 3] != ':') {
        return false;
    }
    int off_h = ts_digits(s + i + 1, 2), off_m = ts_digits(s + i + 4, 2);
    return off_h >= 0 && off_h <= 23 && off_m >= 0 && off_m <= 59;
}

/* Get human-readable language name */
const char *get_language_name(const char *lang_code) {
    if (!lang_code || strlen(lang_code) != 3) {
        return NULL;
    }

    char lower_code[4];
    for (int i = 0; i < 3; i++) {
        lower_code[i] = (char)tolower((unsigned char)lang_code[i]);
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
                lower_code[i] = (char)tolower((unsigned char)lang_input[i]);
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
#define UTF8_INVALID 0xFFFFFFFFu
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

/* Strict UTF-8 (RFC 3629): returns the sequence length and sets *cp. An invalid,
 * overlong, surrogate, out-of-range or truncated sequence returns 1 with
 * *cp = UTF8_INVALID (never an emoji), so the byte is kept as it is. A byte past
 * s[0] is read only while the previous one was a valid continuation byte, so a
 * NUL-terminated string is never read beyond its terminator (PLAN_OPEN_ISSUES.md C). */
static int utf8_decode(const unsigned char *s, unsigned int *cp) {
    unsigned char c = s[0];
    int len;
    unsigned int v, min;
    if (c < 0x80) {
        *cp = c;
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        len = 2; v = c & 0x1Fu; min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3; v = c & 0x0Fu; min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4; v = c & 0x07u; min = 0x10000;
    } else {
        *cp = UTF8_INVALID;
        return 1;
    }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *cp = UTF8_INVALID;
            return 1;
        }
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        *cp = UTF8_INVALID;
        return 1;
    }
    *cp = v;
    return len;
}

/* Strip emoji and shortcodes from text */
static bool ascii_alnum(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static bool shortcode_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-';
}

static bool c_locale_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* Length of a shortcode such as ":smile:" starting at in[pos], or 0. The name is
 * [a-z0-9_+-]+ with at least one lowercase letter, and neither colon may touch an
 * ASCII letter or digit, so times (10:30), ratios (3:1), URLs (http://), IPv6
 * literals, "Note:" and ":)" are not shortcodes. Specification: tests/strip_ref.py. */
static size_t shortcode_at(const char *in, size_t pos, size_t len) {
    if (pos > 0 && ascii_alnum((unsigned char)in[pos - 1])) {
        return 0;
    }
    size_t j = pos + 1;
    bool lower = false;
    while (j < len && shortcode_char((unsigned char)in[j])) {
        if (in[j] >= 'a' && in[j] <= 'z') {
            lower = true;
        }
        j++;
    }
    if (j == pos + 1 || !lower || j >= len || in[j] != ':') {
        return 0;
    }
    if (j + 1 < len && ascii_alnum((unsigned char)in[j + 1])) {
        return 0;
    }
    return j + 1 - pos;
}

/* Never writes more bytes than it reads, so output_size = strlen(input) + 1 always
 * fits. Returns -1 instead of truncating if output_size is smaller than needed. */
int strip_emoji_and_shortcodes(const char *input, char *output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return -1;
    }

    size_t in_len = strlen(input);
    size_t out_pos = 0;
    size_t in_pos = 0;
    bool last_was_space = false;

    while (in_pos < in_len) {
        unsigned char c = (unsigned char)input[in_pos];

        if (c == ':') {
            size_t skip = shortcode_at(input, in_pos, in_len);
            if (skip) {
                in_pos += skip;
                continue;
            }
        }

        unsigned int codepoint;
        int char_len = utf8_decode((const unsigned char *)&input[in_pos], &codepoint);
        if (codepoint != UTF8_INVALID && is_emoji_codepoint(codepoint)) {
            in_pos += (size_t)char_len;
            continue;
        }

        if (c_locale_space(c)) {
            if (c == '\n') {
                if (out_pos + 1 >= output_size) return -1;
                output[out_pos++] = '\n';
                last_was_space = false;   /* Preserve newlines */
            } else if (!last_was_space && out_pos > 0) {
                if (out_pos + 1 >= output_size) return -1;
                output[out_pos++] = ' ';   /* Normalize other whitespace to one space */
                last_was_space = true;
            }
            in_pos++;
            continue;
        }

        if (out_pos + (size_t)char_len >= output_size) return -1;   /* never cut a character */
        memcpy(output + out_pos, input + in_pos, (size_t)char_len);
        out_pos += (size_t)char_len;
        in_pos += (size_t)char_len;
        last_was_space = false;
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

    if (in_pos < in_len) {
        return -1;   /* output_size too small: never truncate silently */
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
