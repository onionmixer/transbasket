/**
 * Script classifier for source_hint (PLAN_SOURCE_HINT_METADATA.md section 5).
 * Mirrors tests/source_hint_ref.py function by function; see include/source_hint.h.
 */

#include <string.h>
#include "source_hint.h"
#include "source_hint_table.h"

#define INVALID_CP (-1)

/* Groups after the Han+Kana merge: the 13 buckets plus Japanese. */
enum { G_JAPANESE = SH_BUCKETS, G_COUNT };

size_t source_hint_table_size(void) {
    return sizeof(SOURCE_HINT_TABLE) / sizeof(SOURCE_HINT_TABLE[0]);
}

const SourceHintRange *source_hint_table(void) {
    return SOURCE_HINT_TABLE;
}

/* Unicode Table 3-7; an ill-formed sequence consumes exactly one byte. */
size_t source_hint_decode(const unsigned char *data, size_t len, size_t pos, int32_t *cp) {
    unsigned char b = data[pos];
    size_t need;
    unsigned char lo = 0x80, hi = 0xBF;

    if (b < 0x80) { *cp = b; return 1; }
    if (b >= 0xC2 && b <= 0xDF) need = 1;
    else if (b == 0xE0) { need = 2; lo = 0xA0; }
    else if ((b >= 0xE1 && b <= 0xEC) || b == 0xEE || b == 0xEF) need = 2;
    else if (b == 0xED) { need = 2; hi = 0x9F; }      /* no surrogates */
    else if (b == 0xF0) { need = 3; lo = 0x90; }
    else if (b >= 0xF1 && b <= 0xF3) need = 3;
    else if (b == 0xF4) { need = 3; hi = 0x8F; }      /* <= U+10FFFF */
    else { *cp = INVALID_CP; return 1; }

    int32_t value = b & (0x7F >> (need + 1));
    for (size_t k = 1; k <= need; k++) {
        if (pos + k >= len) { *cp = INVALID_CP; return 1; }
        unsigned char c = data[pos + k];
        if (k == 1 ? (c < lo || c > hi) : (c < 0x80 || c > 0xBF)) { *cp = INVALID_CP; return 1; }
        value = (value << 6) | (c & 0x3F);
    }
    *cp = value;
    return need + 1;
}

bool source_hint_utf8_valid(const unsigned char *data, size_t len) {
    for (size_t pos = 0; pos < len; ) {
        int32_t cp;
        pos += source_hint_decode(data, len, pos, &cp);
        if (cp == INVALID_CP) return false;
    }
    return true;
}

SourceHintClass source_hint_class_of(int32_t cp) {
    if (cp == INVALID_CP) return SH_IGNORE;
    size_t lo = 0, hi = source_hint_table_size();
    while (lo < hi) {                       /* half-open binary search */
        size_t mid = lo + (hi - lo) / 2;
        const SourceHintRange *r = &SOURCE_HINT_TABLE[mid];
        if ((uint32_t)cp < r->first) hi = mid;
        else if ((uint32_t)cp > r->last) lo = mid + 1;
        else return (SourceHintClass)r->cls;
    }
    return SH_IGNORE;
}

/* Latin: one unit per run of Latin letters, combining marks continue a run.
 * Everything else counted per code point; Ignore, INVALID and stray marks end
 * a run and count nothing. */
static void count_units(const unsigned char *data, size_t len, uint64_t counts[SH_BUCKETS]) {
    memset(counts, 0, sizeof(uint64_t) * SH_BUCKETS);
    bool in_latin = false;
    for (size_t pos = 0; pos < len; ) {
        int32_t cp;
        pos += source_hint_decode(data, len, pos, &cp);
        SourceHintClass c = source_hint_class_of(cp);
        if (c == SH_LATIN) {
            if (!in_latin) counts[SH_LATIN]++;
            in_latin = true;
        } else if (c == SH_MARK && in_latin) {
            /* continues the word */
        } else {
            in_latin = false;
            if (c < SH_BUCKETS) counts[c]++;
        }
    }
}

/* Han+Kana become one Japanese group when merge_japanese is set. */
static void merge_groups(const uint64_t counts[SH_BUCKETS], bool merge_japanese, uint64_t g[G_COUNT]) {
    for (int i = 0; i < SH_BUCKETS; i++) g[i] = counts[i];
    g[G_JAPANESE] = 0;
    if (merge_japanese) {
        g[G_JAPANESE] = counts[SH_KANA] + counts[SH_HAN];
        g[SH_KANA] = 0;
        g[SH_HAN] = 0;
    }
}

static const char *top_lang(int group) {
    switch (group) {
    case SH_HANGUL:   return "kor";
    case G_JAPANESE:  return "jpn";
    case SH_HAN:      return "chi";
    case SH_THAI:     return "tha";
    case SH_GREEK:    return "gre";
    case SH_GEORGIAN: return "geo";
    case SH_ARMENIAN: return "arm";
    default:          return "und";   /* shared scripts and Other */
    }
}

int source_hint_classify(const unsigned char *data, size_t len, SourceHintGuess *out) {
    if (!out || (len && !data) || len > SOURCE_HINT_MAX_INPUT) return -1;
    uint64_t counts[SH_BUCKETS], g[G_COUNT];
    count_units(data, len, counts);
    merge_groups(counts, counts[SH_KANA] > 0, g);

    uint64_t total = 0, top = 0;
    int top_group = -1, top_ties = 0;
    for (int i = 0; i < G_COUNT; i++) {
        total += g[i];
        if (g[i] == 0) continue;
        if (g[i] > top) { top = g[i]; top_group = i; top_ties = 1; }
        else if (g[i] == top) top_ties++;
    }
    if (total == 0) {
        memcpy(out->lang, "und", 4);
        out->mixed = false;
        return 0;
    }
    /* Second element of the values sorted in descending order. */
    uint64_t second = 0;
    if (top_ties >= 2) {
        second = top;
    } else {
        for (int i = 0; i < G_COUNT; i++)
            if (g[i] < top && g[i] > second) second = g[i];
    }
    out->mixed = 100 * second >= 30 * total;
    memcpy(out->lang, top_ties >= 2 ? "und" : top_lang(top_group), 4);
    return 0;
}

typedef struct {
    const char *lang;
    unsigned mask;   /* bit per SourceHintClass */
} RequiredScripts;

#define BIT(c) (1u << (c))

static const RequiredScripts REQUIRED[] = {
    { "kor", BIT(SH_HANGUL) | BIT(SH_HAN) }, { "jpn", BIT(SH_KANA) | BIT(SH_HAN) },
    { "chi", BIT(SH_HAN) }, { "zhs", BIT(SH_HAN) }, { "zht", BIT(SH_HAN) },
    { "tha", BIT(SH_THAI) }, { "gre", BIT(SH_GREEK) }, { "heb", BIT(SH_HEBREW) },
    { "yid", BIT(SH_HEBREW) }, { "geo", BIT(SH_GEORGIAN) }, { "arm", BIT(SH_ARMENIAN) },
    { "rus", BIT(SH_CYRILLIC) }, { "ukr", BIT(SH_CYRILLIC) }, { "bul", BIT(SH_CYRILLIC) },
    { "bel", BIT(SH_CYRILLIC) },
};

typedef struct {
    int group;
    const char *allowed[2];
} DominantScript;

static const DominantScript DOMINANT[] = {
    { SH_HANGUL, { "kor", NULL } }, { G_JAPANESE, { "jpn", NULL } }, { SH_THAI, { "tha", NULL } },
    { SH_GREEK, { "gre", NULL } }, { SH_GEORGIAN, { "geo", NULL } }, { SH_ARMENIAN, { "arm", NULL } },
    { SH_HEBREW, { "heb", "yid" } },
};

bool source_hint_model_contradicted(const unsigned char *data, size_t len, const char *lang) {
    if (!lang || (len && !data) || len > SOURCE_HINT_MAX_INPUT) return false;
    uint64_t counts[SH_BUCKETS], g[G_COUNT];
    count_units(data, len, counts);

    for (size_t i = 0; i < sizeof(REQUIRED) / sizeof(REQUIRED[0]); i++) {
        if (strcmp(REQUIRED[i].lang, lang) != 0) continue;
        uint64_t have = 0;
        for (int c = 0; c < SH_BUCKETS; c++)
            if (REQUIRED[i].mask & BIT(c)) have += counts[c];
        if (have == 0) return true;
        break;
    }

    /* Japanese is dominant only with real Kana evidence: Kana >= 20% of Han+Kana. */
    bool merge = counts[SH_KANA] > 0 && 5 * counts[SH_KANA] >= counts[SH_KANA] + counts[SH_HAN];
    merge_groups(counts, merge, g);
    uint64_t total = 0;
    for (int i = 0; i < G_COUNT; i++) total += g[i];
    if (total == 0) return false;

    for (size_t i = 0; i < sizeof(DOMINANT) / sizeof(DOMINANT[0]); i++) {
        const DominantScript *d = &DOMINANT[i];
        if (100 * g[d->group] < 95 * total) continue;
        bool allowed = false;
        for (int k = 0; k < 2 && d->allowed[k]; k++)
            if (strcmp(d->allowed[k], lang) == 0) allowed = true;
        if (!allowed) return true;
    }
    return false;
}
