#ifndef STRICT_JSON_H
#define STRICT_JSON_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Strict validator for codex structured output (PLAN_SOURCE_HINT_METADATA.md
 * section 4.3). tests/strict_json_ref.py is the normative specification and
 * tests/test_strict_json.py compares this implementation with it, verdict and
 * reason. cJSON is only used on input this function accepted: cJSON 1.7.15
 * decodes invalid \u escapes to NUL, accepts control bytes, raw LF in strings,
 * numbers like 01 and a BOM, and its default lookup ignores case.
 */

typedef enum {
    SJ_ACCEPT = 0,
    SJ_UTF8,          /* input is not strict UTF-8 (checked first) */
    SJ_SYNTAX,        /* RFC 8259 grammar */
    SJ_NUL,           /* a \u escape decodes to U+0000 */
    SJ_SURROGATE,     /* unpaired surrogate escape */
    SJ_DEPTH,         /* more than STRICT_JSON_MAX_DEPTH arrays/objects open */
    SJ_ROOT,          /* root value is not an object */
    SJ_DUPLICATE,     /* a reserved key occurs twice among the root members */
    SJ_TRANSLATION    /* root has no string "translation" member */
} StrictJsonResult;

#define STRICT_JSON_MAX_DEPTH 32

/* Validate data[0..len). Reserved root keys: translation, source_language, mixed. */
StrictJsonResult strict_json_validate(const unsigned char *data, size_t len);

/* "accept", "utf8", "syntax", ... (the reference's reason names). */
const char *strict_json_reason(StrictJsonResult result);

/*
 * true when cJSON 1.7.15 would decode some string or key in data[0..len) to
 * contain U+0000 (PLAN_CLEANUP_DAYS_AND_NUL.md section 4; tests/cjson_nul_ref.py
 * is the specification). cJSON turns every backslash-u escape whose four
 * following bytes are not all hex digits, or are 0000, into a NUL and the C
 * string is then silently cut there. Call it on the bytes cJSON consumed:
 * cJSON_ParseWithOpts(x, &end, 0) succeeded -> json_decodes_nul(x, end - x).
 * Over that range the answer is exact. Scanning stops at a raw NUL byte (cJSON
 * reads a C string).
 */
bool json_decodes_nul(const unsigned char *data, size_t len);

#endif /* STRICT_JSON_H */
