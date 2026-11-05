/**
 * Strict JSON validator for codex structured output; mirrors the scanner in
 * tests/strict_json_ref.py rule by rule (see include/strict_json.h).
 *
 * The first violation in scan order decides the reason. Because the whole
 * input is checked to be strict UTF-8 first, every byte >= 0x80 belongs to a
 * well-formed sequence and only ASCII bytes are structural.
 */

#include <stdbool.h>
#include <string.h>
#include "strict_json.h"
#include "source_hint.h"

enum { V_OBJECT, V_ARRAY, V_STRING, V_NUMBER, V_TRUE, V_FALSE, V_NULL };

static const char *const RESERVED[] = { "translation", "source_language", "mixed" };
#define N_RESERVED 3
#define KEY_MAX 32   /* longer decoded keys cannot be reserved */

typedef struct {
    const unsigned char *s;
    size_t len, i;
    int depth;
    StrictJsonResult error;
    /* root members, for the semantic layer */
    int reserved_count[N_RESERVED];
    int translation_type;      /* value type of the (single) root "translation", -1 if absent */
} Scanner;

static bool is_ws(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int peek(const Scanner *sc) { return sc->i < sc->len ? sc->s[sc->i] : -1; }
static void ws(Scanner *sc) { while (sc->i < sc->len && is_ws(sc->s[sc->i])) sc->i++; }
static bool fail(Scanner *sc, StrictJsonResult r) { if (sc->error == SJ_ACCEPT) sc->error = r; return false; }
static bool expect(Scanner *sc, int ch) {
    if (peek(sc) != ch) return fail(sc, SJ_SYNTAX);
    sc->i++;
    return true;
}

/* Exactly 4 hex digits at sc->i. */
static bool hex4(Scanner *sc, unsigned *out) {
    if (sc->len - sc->i < 4) return fail(sc, SJ_SYNTAX);
    unsigned v = 0;
    for (int k = 0; k < 4; k++) {
        int h = hexval(sc->s[sc->i + k]);
        if (h < 0) return fail(sc, SJ_SYNTAX);
        v = v * 16 + (unsigned)h;
    }
    sc->i += 4;
    *out = v;
    return true;
}

static void put_utf8(unsigned cp, unsigned char *key, size_t *klen, bool *overflow) {
    unsigned char b[4];
    size_t n;
    if (cp < 0x80) { b[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (unsigned char)(0xC0 | (cp >> 6)); b[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = (unsigned char)(0xE0 | (cp >> 12)); b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                             b[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3; }
    else { b[0] = (unsigned char)(0xF0 | (cp >> 18)); b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
           b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4; }
    if (*klen + n > KEY_MAX) { *overflow = true; return; }
    memcpy(key + *klen, b, n);
    *klen += n;
}

/* A string at sc->i. When key != NULL the decoded value is stored there (up to
 * KEY_MAX bytes; *overflow set beyond that). */
static bool string(Scanner *sc, unsigned char *key, size_t *klen, bool *overflow) {
    if (!expect(sc, '"')) return false;
    for (;;) {
        if (sc->i >= sc->len) return fail(sc, SJ_SYNTAX);
        unsigned char c = sc->s[sc->i++];
        if (c == '"') return true;
        if (c < 0x20) return fail(sc, SJ_SYNTAX);
        if (c != '\\') {
            if (key) {            /* raw bytes are already well-formed UTF-8 */
                if (*klen + 1 > KEY_MAX) *overflow = true;
                else key[(*klen)++] = c;
            }
            continue;
        }
        int e = peek(sc);
        sc->i++;
        unsigned cp;
        switch (e) {
        case '"': cp = '"'; break;
        case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            if (!hex4(sc, &cp)) return false;
            if (cp == 0) return fail(sc, SJ_NUL);
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                /* Pair only if the next 6 bytes are exactly \u + 4 hex digits
                 * in DC00-DFFF; otherwise this high surrogate is the violation. */
                const unsigned char *n = sc->s + sc->i;
                bool pair = sc->len - sc->i >= 6 && n[0] == '\\' && n[1] == 'u';
                unsigned low = 0;
                for (int k = 2; pair && k < 6; k++) {
                    int h = hexval(n[k]);
                    if (h < 0) pair = false;
                    else low = low * 16 + (unsigned)h;
                }
                if (!pair || low < 0xDC00 || low > 0xDFFF) return fail(sc, SJ_SURROGATE);
                sc->i += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return fail(sc, SJ_SURROGATE);
            }
            break;
        default:
            return fail(sc, SJ_SYNTAX);
        }
        if (key) put_utf8(cp, key, klen, overflow);
    }
}

static bool number(Scanner *sc) {
    if (peek(sc) == '-') sc->i++;
    int c = peek(sc);
    if (c == '0') {
        sc->i++;
    } else if (c >= '1' && c <= '9') {
        while (sc->i < sc->len && is_digit(sc->s[sc->i])) sc->i++;
    } else {
        return fail(sc, SJ_SYNTAX);
    }
    if (peek(sc) == '.') {
        sc->i++;
        if (!(sc->i < sc->len && is_digit(sc->s[sc->i]))) return fail(sc, SJ_SYNTAX);
        while (sc->i < sc->len && is_digit(sc->s[sc->i])) sc->i++;
    }
    if (peek(sc) == 'e' || peek(sc) == 'E') {
        sc->i++;
        if (peek(sc) == '+' || peek(sc) == '-') sc->i++;
        if (!(sc->i < sc->len && is_digit(sc->s[sc->i]))) return fail(sc, SJ_SYNTAX);
        while (sc->i < sc->len && is_digit(sc->s[sc->i])) sc->i++;
    }
    return true;
}

static bool value(Scanner *sc, int *type);

static bool object(Scanner *sc) {
    bool root = sc->depth == 1;
    if (!expect(sc, '{')) return false;
    ws(sc);
    if (peek(sc) == '}') { sc->i++; return true; }
    for (;;) {
        ws(sc);
        if (peek(sc) != '"') return fail(sc, SJ_SYNTAX);
        unsigned char key[KEY_MAX];
        size_t klen = 0;
        bool overflow = false;
        if (!string(sc, root ? key : NULL, &klen, &overflow)) return false;
        ws(sc);
        if (!expect(sc, ':')) return false;
        ws(sc);
        int type;
        if (!value(sc, &type)) return false;
        if (root && !overflow) {
            for (int r = 0; r < N_RESERVED; r++) {
                if (klen == strlen(RESERVED[r]) && memcmp(key, RESERVED[r], klen) == 0) {
                    sc->reserved_count[r]++;
                    if (r == 0) sc->translation_type = type;
                }
            }
        }
        ws(sc);
        if (peek(sc) == ',') { sc->i++; continue; }
        return expect(sc, '}');
    }
}

static bool array(Scanner *sc) {
    if (!expect(sc, '[')) return false;
    ws(sc);
    if (peek(sc) == ']') { sc->i++; return true; }
    for (;;) {
        ws(sc);
        int type;
        if (!value(sc, &type)) return false;
        ws(sc);
        if (peek(sc) == ',') { sc->i++; continue; }
        return expect(sc, ']');
    }
}

static bool literal(Scanner *sc, const char *word) {
    size_t n = strlen(word);
    if (sc->len - sc->i >= n && memcmp(sc->s + sc->i, word, n) == 0) { sc->i += n; return true; }
    return false;
}

static bool value(Scanner *sc, int *type) {
    int c = peek(sc);
    if (c == '{' || c == '[') {
        /* depth is checked when a container OPENS, so recursion stays shallow */
        if (++sc->depth > STRICT_JSON_MAX_DEPTH) return fail(sc, SJ_DEPTH);
        bool ok = c == '{' ? object(sc) : array(sc);
        sc->depth--;
        *type = c == '{' ? V_OBJECT : V_ARRAY;
        return ok;
    }
    if (c == '"') { *type = V_STRING; return string(sc, NULL, NULL, NULL); }
    if (c == '-' || (c >= '0' && c <= '9')) { *type = V_NUMBER; return number(sc); }
    if (literal(sc, "true")) { *type = V_TRUE; return true; }
    if (literal(sc, "false")) { *type = V_FALSE; return true; }
    if (literal(sc, "null")) { *type = V_NULL; return true; }
    return fail(sc, SJ_SYNTAX);
}

StrictJsonResult strict_json_validate(const unsigned char *data, size_t len) {
    if (len && !data) return SJ_SYNTAX;
    if (!source_hint_utf8_valid(data, len)) return SJ_UTF8;
    Scanner sc = { .s = data, .len = len, .translation_type = -1 };
    ws(&sc);
    int root_type;
    if (!value(&sc, &root_type)) return sc.error;
    ws(&sc);
    if (sc.i != sc.len) return SJ_SYNTAX;
    if (root_type != V_OBJECT) return SJ_ROOT;
    for (int r = 0; r < N_RESERVED; r++)
        if (sc.reserved_count[r] > 1) return SJ_DUPLICATE;
    if (sc.translation_type != V_STRING) return SJ_TRANSLATION;
    return SJ_ACCEPT;
}

const char *strict_json_reason(StrictJsonResult result) {
    static const char *const names[] = { "accept", "utf8", "syntax", "nul", "surrogate", "depth",
                                         "root", "duplicate", "translation" };
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "?";
}

/* Not a JSON parser: backslashes pair with the next byte, which is how cJSON
 * finds the end of a string; inside the root value cJSON accepted they only
 * occur in strings, and hex digits contain neither a backslash nor a quote, so
 * the pairing agrees with cJSON's decoder up to the first escape that yields
 * NUL. */
bool json_decodes_nul(const unsigned char *data, size_t len) {
    if (!data) return false;
    const unsigned char *nul = memchr(data, 0, len);
    if (nul) len = (size_t)(nul - data);
    size_t i = 0;
    while (i < len) {
        if (data[i] != '\\') { i++; continue; }
        if (i + 1 < len && data[i + 1] == 'u') {
            if (len - i < 6) return true;          /* fewer than four bytes follow */
            unsigned v = 0;
            for (size_t k = 2; k < 6; k++) {
                int h = hexval(data[i + k]);
                if (h < 0) return true;            /* cJSON's parse_hex4 yields 0 */
                v = v * 16 + (unsigned)h;
            }
            if (v == 0) return true;
            i += 6;
        } else {
            i += 2;
        }
    }
    return false;
}
