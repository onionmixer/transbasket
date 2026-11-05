#ifndef STRICT_JSON_H
#define STRICT_JSON_H

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

#endif /* STRICT_JSON_H */
