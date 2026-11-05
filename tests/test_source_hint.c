/*
 * Harness for tests/test_source_hint.py (differential test against
 * tests/source_hint_ref.py).
 *
 *   test_source_hint --dump-table      one "class first last" line per range
 *   test_source_hint < records         records: u32 LE length, bytes, 3-byte model
 *                                      code or "---"; one result line each
 *
 * Result line: lang mixed contradicted(0/1/-) utf8_valid checksum
 * checksum = sum over decoded code points (cp + 2) * (index + 1), mod 2^64,
 * with ill-formed sequences counted as cp = -1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "source_hint.h"

static const char *CLASS_NAMES[] = {
    "Hangul", "Kana", "Han", "Latin", "Cyrillic", "Greek", "Hebrew", "Arabic",
    "Thai", "Devanagari", "Georgian", "Armenian", "Other", "Mark", "Ignore"
};

static int read_exact(void *buf, size_t n) {
    return n == 0 || fread(buf, 1, n, stdin) == n;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--dump-table") == 0) {
        const SourceHintRange *t = source_hint_table();
        for (size_t i = 0; i < source_hint_table_size(); i++)
            printf("%s %u %u\n", CLASS_NAMES[t[i].cls], t[i].first, t[i].last);
        return 0;
    }

    unsigned char lenbuf[4];
    while (read_exact(lenbuf, 4)) {
        size_t len = (size_t)lenbuf[0] | (size_t)lenbuf[1] << 8 | (size_t)lenbuf[2] << 16 | (size_t)lenbuf[3] << 24;
        unsigned char *data = malloc(len ? len : 1);
        char lang[4] = {0};
        if (!data || !read_exact(data, len) || !read_exact(lang, 3)) {
            fprintf(stderr, "truncated record\n");
            free(data);
            return 2;
        }

        SourceHintGuess guess = { "ERR", false };   /* classify leaves it untouched on error */
        if (source_hint_classify(data, len, &guess) != 0) { strcpy(guess.lang, "ERR"); guess.mixed = false; }

        char contra = '-';
        if (strcmp(lang, "---") != 0)
            contra = source_hint_model_contradicted(data, len, lang) ? '1' : '0';

        unsigned long long checksum = 0, index = 0;
        for (size_t pos = 0; pos < len; ) {
            int32_t cp;
            pos += source_hint_decode(data, len, pos, &cp);
            checksum += (unsigned long long)((long long)cp + 2) * (index + 1);
            index++;
        }

        printf("%s %d %c %d %llu\n", guess.lang, guess.mixed ? 1 : 0, contra,
               source_hint_utf8_valid(data, len) ? 1 : 0, checksum);
        free(data);
    }
    return 0;
}
