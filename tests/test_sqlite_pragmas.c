/* Harness for tests/test_sqlite_pragmas.py (PLAN_CODE_ISSUES_4.md 1): open the SQLite
 * backend with the given options and print what is in effect on the backend's OWN
 * connection (synchronous is per connection, so another connection cannot see it).
 *   <db path> <journal or -> <synchronous or ->   ("-" = NULL)
 * prints "journal_mode=<x> synchronous=<n>" or "init failed". Log lines go to stdout too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "trans_cache.h"
#include "cache_backend_sqlite.h"

static void pragma(sqlite3 *db, const char *sql, char *out, size_t n) {
    sqlite3_stmt *st = NULL;
    snprintf(out, n, "?");
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        snprintf(out, n, "%s", (const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    SqliteCacheOptions o = { .journal_mode = strcmp(argv[2], "-") ? argv[2] : NULL,
                             .synchronous = strcmp(argv[3], "-") ? argv[3] : NULL,
                             .keep_journal_mode = getenv("KEEP_JOURNAL") != NULL };
    TransCache *c = trans_cache_init_with_backend(CACHE_BACKEND_SQLITE, argv[1],
                                                  (o.journal_mode || o.synchronous || o.keep_journal_mode) ? &o : NULL);
    if (!c) {
        printf("init failed\n");
        return 0;
    }
    SqliteBackendContext *ctx = (SqliteBackendContext *)c->backend_ctx;
    char jm[32], sy[32];
    pragma(ctx->db, "PRAGMA journal_mode", jm, sizeof jm);
    pragma(ctx->db, "PRAGMA synchronous", sy, sizeof sy);
    printf("journal_mode=%s synchronous=%s\n", jm, sy);
    trans_cache_free(c);
    return 0;
}
