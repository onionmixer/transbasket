#ifndef SOURCE_HINT_H
#define SOURCE_HINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Script classifier for the source_hint metadata of from=auto translations
 * (PLAN_SOURCE_HINT_METADATA.md section 5). tests/source_hint_ref.py is the
 * normative specification; tests/test_source_hint.py compares this
 * implementation with it byte string by byte string.
 *
 * Inputs are a pointer plus a length (embedded NUL bytes are data) of at most
 * SOURCE_HINT_MAX_INPUT bytes, which keeps every 100 * count product far
 * below UINT64_MAX.
 */

#define SOURCE_HINT_VERSION 1
#define SOURCE_HINT_MAX_INPUT 10000   /* MAX_TEXT_LENGTH */

/* Classes of the generated range table (src/source_hint_table.h). */
typedef enum {
    SH_HANGUL = 0, SH_KANA, SH_HAN, SH_LATIN, SH_CYRILLIC, SH_GREEK, SH_HEBREW,
    SH_ARABIC, SH_THAI, SH_DEVANAGARI, SH_GEORGIAN, SH_ARMENIAN, SH_OTHER,
    SH_BUCKETS,          /* number of counted buckets */
    SH_MARK = SH_BUCKETS,
    SH_IGNORE
} SourceHintClass;

typedef struct {
    uint32_t first, last;
    unsigned char cls;   /* SourceHintClass */
} SourceHintRange;

typedef struct {
    char lang[4];        /* "kor", "jpn", "chi", ..., or "und" */
    bool mixed;
} SourceHintGuess;

/* Strict UTF-8 decode of one code point at data[pos]. Returns the number of
 * bytes consumed (1 for an ill-formed sequence, with *cp = -1). pos < len. */
size_t source_hint_decode(const unsigned char *data, size_t len, size_t pos, int32_t *cp);

/* true if data is entirely well-formed UTF-8. */
bool source_hint_utf8_valid(const unsigned char *data, size_t len);

/* Class of one code point (-1 = ill-formed, classified as Ignore). */
SourceHintClass source_hint_class_of(int32_t cp);

/* Fallback hint. Returns -1 if len exceeds SOURCE_HINT_MAX_INPUT. */
int source_hint_classify(const unsigned char *data, size_t len, SourceHintGuess *out);

/* true only when the script evidence rules out the model's code `lang`
 * (a canonical code or "und"). Inputs longer than SOURCE_HINT_MAX_INPUT are
 * never contradicted. */
bool source_hint_model_contradicted(const unsigned char *data, size_t len, const char *lang);

/* Access to the range table, for the table-identity test. */
size_t source_hint_table_size(void);
const SourceHintRange *source_hint_table(void);

#endif /* SOURCE_HINT_H */
