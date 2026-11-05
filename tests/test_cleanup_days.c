/*
 * Harness for tests/test_cleanup_days.py (PLAN_CLEANUP_DAYS_AND_NUL.md section 5 V5).
 * The python driver computes every expected value; this program only reports.
 *
 *   test_cleanup_days arith                      stdin: "cutoff <now> <days>" | "interval <days>"
 *   test_cleanup_days prepare sqlite <path> <n>  adds rows s1..sN (driver then sets last_used)
 *   test_cleanup_days run <text|sqlite> <path> <days>
 *                                                prints "expired=<stats> removed=<cleanup>", then saves
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "trans_cache.h"

static int arith(void) {
    char op[16];
    while (scanf("%15s", op) == 1) {
        if (strcmp(op, "cutoff") == 0) {
            int64_t now; int days;
            if (scanf("%" SCNd64 " %d", &now, &days) != 2) return 2;
            printf("%" PRId64 "\n", trans_cache_cutoff(now, days));
        } else if (strcmp(op, "interval") == 0) {
            int days;
            if (scanf("%d", &days) != 1) return 2;
            printf("%d\n", trans_cache_cleanup_interval(days));
        } else {
            return 2;
        }
    }
    return 0;
}

static TransCache *open_cache(const char *backend, const char *path) {
    CacheBackendType type = strcmp(backend, "sqlite") == 0 ? CACHE_BACKEND_SQLITE : CACHE_BACKEND_TEXT;
    return trans_cache_init_with_backend(type, path, NULL);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "arith") == 0) return arith();
    if (argc == 5 && strcmp(argv[1], "prepare") == 0) {
        TransCache *c = open_cache(argv[2], argv[3]);
        if (!c) return 2;
        int n = atoi(argv[4]);
        for (int i = 1; i <= n; i++) {
            char src[32];
            snprintf(src, sizeof(src), "s%d", i);
            if (trans_cache_add(c, "kor", "eng", src, "t") != 0) { trans_cache_free(c); return 2; }
        }
        trans_cache_save(c);
        trans_cache_free(c);
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "run") == 0) {
        TransCache *c = open_cache(argv[2], argv[3]);
        if (!c) return 2;
        int days = atoi(argv[4]);   /* the driver passes only in-range ints */
        size_t total = 0, active = 0, expired = 0;
        trans_cache_stats(c, &total, &active, &expired, 1, days);
        int removed = trans_cache_cleanup(c, days);
        printf("expired=%zu removed=%d\n", expired, removed);
        int rc = trans_cache_save(c) == 0 ? 0 : 2;
        trans_cache_free(c);
        return rc;
    }
    fprintf(stderr, "usage: see file header\n");
    return 2;
}
