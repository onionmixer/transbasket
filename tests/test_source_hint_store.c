/*
 * Storage tests for source_hint (PLAN_SOURCE_HINT_METADATA.md sections 6, 7, 11).
 * Runs against the real text and SQLite backends in a temporary directory.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sqlite3.h>
#include "trans_cache.h"
#include "cache_backend_sqlite.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static char dir[] = "/tmp/source-hint-store-XXXXXX";

static SourceHint H(const char *lang, char origin, int version, bool mixed) {
    SourceHint h = { .mixed = mixed, .origin = origin, .version = version };
    memcpy(h.lang, lang, 4);
    return h;
}

static sqlite3 *raw_open(const char *path) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) { printf("raw open failed\n"); exit(2); }
    return db;
}

static long long raw_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    long long v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static char *raw_text(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    char *v = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW &&
        sqlite3_column_text(st, 0))
        v = strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return v;
}

static TransCache *open_sqlite(const char *path) {
    TransCache *c = trans_cache_init_with_backend(CACHE_BACKEND_SQLITE, path, NULL);
    if (!c) { printf("sqlite init failed for %s\n", path); exit(2); }
    return c;
}

static TranslationControl expired_control(void) {
    TranslationControl c;
    clock_gettime(CLOCK_MONOTONIC, &c.deadline);
    c.deadline.tv_sec -= 1;
    c.cancelled = NULL;
    return c;
}

static size_t code_count(void) {
    size_t n = 0;
    while (iso_639_2_code_at(n)) n++;
    return n;
}

static void test_new_db_and_basic_roundtrip(void) {
    char path[256];
    snprintf(path, sizeof path, "%s/new.db", dir);
    TransCache *c = open_sqlite(path);
    sqlite3 *db = raw_open(path);
    CHECK(raw_int(db, "PRAGMA user_version") == 2, "user_version");
    CHECK(raw_int(db, "SELECT count(*) FROM sqlite_master WHERE name IN ('trans_cache_source_hint',"
                      "'trans_cache_source_hint_delete','trans_cache_hint_code','trans_cache_effective_source')") == 4,
          "owned objects");
    CHECK(raw_int(db, "SELECT count(*) FROM trans_cache_hint_code") == (long long)code_count() + 1,
          "code table = %zu codes + und", code_count());

    SourceHint kor = H("kor", SOURCE_HINT_ORIGIN_SCRIPT, 1, false);
    CHECK(trans_cache_store_result(c, "auto", "eng", "안녕하세요", "Hello", NULL, &kor, NULL) == 0, "store add");
    CHECK(trans_cache_store_result(c, "kor", "eng", "감사합니다", "Thanks", NULL, &kor, NULL) == 0, "store explicit");
    CHECK(raw_int(db, "SELECT count(*) FROM trans_cache_source_hint") == 1, "explicit request stores no hint");

    CacheEntry *e = trans_cache_lookup(c, "auto", "eng", "안녕하세요");
    CHECK(e && e->hint.origin == SOURCE_HINT_ORIGIN_SCRIPT && strcmp(e->hint.lang, "kor") == 0 && e->hint.version == 1,
          "lookup hint");
    /* same translation -> update_count semantics, copy synced after commit */
    CHECK(trans_cache_store_result(c, "auto", "eng", "안녕하세요", "Hello", e, NULL, NULL) == 0, "update_count");
    CHECK(e->count == 2, "copy count %d", e ? e->count : -1);
    CHECK(raw_int(db, "SELECT count FROM trans_cache WHERE from_lang='auto'") == 2, "stored count");
    /* different translation -> update_translation semantics */
    CHECK(trans_cache_store_result(c, "auto", "eng", "안녕하세요", "Hi", e, NULL, NULL) == 0, "update_translation");
    CHECK(e->count == 1 && strcmp(e->translated_text, "Hi") == 0, "copy after update_translation");
    trans_cache_entry_free(e);

    char *view = raw_text(db, "SELECT group_concat(from_lang || ':' || ifnull(effective_source,'NULL'), ',') "
                              "FROM (SELECT * FROM trans_cache_effective_source ORDER BY id)");
    CHECK(view && strcmp(view, "auto:kor,kor:kor") == 0, "view %s", view ? view : "(null)");
    free(view);
    trans_cache_free(c);

    c = open_sqlite(path);            /* restart: hint persists, objects unchanged */
    e = trans_cache_lookup(c, "auto", "eng", "안녕하세요");
    CHECK(e && strcmp(e->hint.lang, "kor") == 0, "hint after restart");
    trans_cache_entry_free(e);
    trans_cache_free(c);
    sqlite3_close(db);
}

static void test_priority_and_invalid(void) {
    char path[256];
    snprintf(path, sizeof path, "%s/prio.db", dir);
    TransCache *c = open_sqlite(path);
    sqlite3 *db = raw_open(path);
    const char *t = "text";
    struct { SourceHint h; const char *expect; } steps[] = {
        { H("kor", SOURCE_HINT_ORIGIN_SCRIPT, 1, false), "kor/script/1" },
        { H("jpn", SOURCE_HINT_ORIGIN_MODEL, 1, true),   "jpn/model/1" },   /* model > script */
        { H("chi", SOURCE_HINT_ORIGIN_SCRIPT, 9, false), "jpn/model/1" },   /* script never replaces model */
        { H("kor", SOURCE_HINT_ORIGIN_MODEL, 1, false),  "kor/model/1" },   /* same-version model: latest */
        { H("eng", SOURCE_HINT_ORIGIN_MODEL, 2, false),  "eng/model/2" },
        { H("jpn", SOURCE_HINT_ORIGIN_MODEL, 1, false),  "eng/model/2" },   /* lower version kept out */
    };
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        CacheEntry *e = trans_cache_lookup(c, "auto", "eng", t);
        CHECK(trans_cache_store_result(c, "auto", "eng", t, "out", e, &steps[i].h, NULL) == 0, "step %zu", i);
        trans_cache_entry_free(e);
        char *got = raw_text(db, "SELECT source_hint || '/' || origin || '/' || hint_version FROM trans_cache_source_hint");
        CHECK(got && strcmp(got, steps[i].expect) == 0, "priority step %zu: %s != %s", i, got ? got : "(null)", steps[i].expect);
        free(got);
    }
    /* invalid stored code: lookup and view see no hint; any valid hint replaces it */
    sqlite3_exec(db, "UPDATE trans_cache_source_hint SET source_hint='xyz', origin='model', hint_version=99", 0, 0, 0);
    CacheEntry *e = trans_cache_lookup(c, "auto", "eng", t);
    CHECK(e && e->hint.origin == SOURCE_HINT_ORIGIN_NONE, "invalid code hidden from lookup");
    char *v = raw_text(db, "SELECT ifnull(effective_source,'NULL') FROM trans_cache_effective_source");
    CHECK(v && strcmp(v, "NULL") == 0, "invalid code hidden from view: %s", v ? v : "(null)");
    free(v);
    SourceHint s1 = H("kor", SOURCE_HINT_ORIGIN_SCRIPT, 1, false);
    CHECK(trans_cache_store_result(c, "auto", "eng", t, "out", e, &s1, NULL) == 0, "store over invalid");
    trans_cache_entry_free(e);
    char *got = raw_text(db, "SELECT source_hint || '/' || origin FROM trans_cache_source_hint");
    CHECK(got && strcmp(got, "kor/script") == 0, "valid hint replaces invalid: %s", got ? got : "(null)");
    free(got);
    /* invalid hints are never stored */
    SourceHint bad = H("xyz", SOURCE_HINT_ORIGIN_MODEL, 1, false);
    CHECK(trans_cache_store_result(c, "auto", "eng", "other", "o", NULL, &bad, NULL) == 0, "store with invalid hint");
    CHECK(raw_int(db, "SELECT count(*) FROM trans_cache_source_hint") == 1, "invalid hint not stored");
    trans_cache_free(c);
    sqlite3_close(db);
}

static void test_vanished_deadline_busy(void) {
    char path[256];
    snprintf(path, sizeof path, "%s/edge.db", dir);
    TransCache *c = open_sqlite(path);
    sqlite3 *db = raw_open(path);
    SourceHint kor = H("kor", SOURCE_HINT_ORIGIN_SCRIPT, 1, false);
    trans_cache_store_result(c, "auto", "eng", "a", "A", NULL, &kor, NULL);

    /* parent vanished after the lookup: the update matches 0 rows -> add again */
    CacheEntry *e = trans_cache_lookup(c, "auto", "eng", "a");
    sqlite3_exec(db, "DELETE FROM trans_cache", 0, 0, 0);
    CHECK(raw_int(db, "SELECT count(*) FROM trans_cache_source_hint") == 0, "trigger removed the hint");
    CHECK(trans_cache_store_result(c, "auto", "eng", "a", "A", e, &kor, NULL) == 0, "vanished -> re-added");
    CHECK(raw_int(db, "SELECT count FROM trans_cache") == 1 && e->count == 1, "re-added count 1");
    CHECK(raw_int(db, "SELECT count(*) FROM trans_cache_source_hint") == 1, "hint re-written");

    /* expired deadline: nothing written, caller copy untouched */
    TranslationControl expired = expired_control();
    int before = e->count;
    CHECK(trans_cache_store_result(c, "auto", "eng", "a", "CHANGED", e, &kor, &expired) == -1, "expired -> -1");
    CHECK(e->count == before && strcmp(e->translated_text, "A") == 0, "copy untouched on failure");
    CHECK(raw_int(db, "SELECT count FROM trans_cache") == 1, "nothing written after deadline");
    CHECK(trans_cache_store_result(c, "auto", "eng", "new", "N", NULL, &kor, &expired) == -1, "expired add -> -1");
    CHECK(raw_int(db, "SELECT count(*) FROM trans_cache") == 1, "no row added after deadline");

    /* another connection holds the write lock: fail fast, no wait */
    sqlite3_exec(db, "BEGIN IMMEDIATE", 0, 0, 0);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = trans_cache_store_result(c, "auto", "eng", "busy", "B", NULL, &kor, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(rc == -1 && ms < 200, "BUSY fails fast (rc %d, %ld ms)", rc, ms);
    sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    CHECK(trans_cache_store_result(c, "auto", "eng", "busy", "B", NULL, &kor, NULL) == 0, "works after release");
    trans_cache_entry_free(e);
    trans_cache_free(c);
    sqlite3_close(db);
}

/* SQLITE_FULL while writing the hint (page limit frozen): whichever way SQLite
 * undoes it, every parent must be applied exactly once. (Measured: here it is
 * a statement-level rollback; the whole-transaction path is covered by
 * test_hint_failure_paths.) */
static void test_full_aborts_then_parent_retry(void) {
    char path[256];
    snprintf(path, sizeof path, "%s/full.db", dir);
    TransCache *c = open_sqlite(path);
    enum { N = 400 };
    char text[32];
    for (int i = 0; i < N; i++) {
        snprintf(text, sizeof text, "row %d", i);
        trans_cache_store_result(c, "auto", "eng", text, "x", NULL, NULL, NULL);
        CacheEntry *e = trans_cache_lookup(c, "auto", "eng", text);   /* count 1 -> 2 so later updates stay in place */
        trans_cache_store_result(c, "auto", "eng", text, "x", e, NULL, NULL);
        trans_cache_entry_free(e);
    }
    SqliteBackendContext *ctx = (SqliteBackendContext *)c->backend_ctx;
    char pragma[64];
    snprintf(pragma, sizeof pragma, "PRAGMA max_page_count=%lld", raw_int(ctx->db, "PRAGMA page_count"));
    sqlite3_exec(ctx->db, pragma, 0, 0, 0);

    int ok = 0;
    for (int i = 0; i < N; i++) {
        snprintf(text, sizeof text, "row %d", i);
        CacheEntry *e = trans_cache_lookup(c, "auto", "eng", text);
        SourceHint kor = H("kor", SOURCE_HINT_ORIGIN_SCRIPT, 1, false);
        if (trans_cache_store_result(c, "auto", "eng", text, "x", e, &kor, NULL) == 0) ok++;
        trans_cache_entry_free(e);
    }
    sqlite3_exec(ctx->db, "PRAGMA max_page_count=1073741823", 0, 0, 0);
    long long hints = raw_int(ctx->db, "SELECT count(*) FROM trans_cache_source_hint");
    long long exact = raw_int(ctx->db, "SELECT count(*) FROM trans_cache WHERE count = 3");
    CHECK(hints > 0 && hints < N, "FULL path reached (hints %lld of %d)", hints, N);
    CHECK(ok == N && exact == N, "every parent applied exactly once (ok %d, count==3 rows %lld)", ok, exact);
    trans_cache_free(c);
}

/* Deterministic fault injection with test-only triggers on the hint table:
 * RAISE(ROLLBACK) ends the WHOLE transaction (parent included) -> the parent
 * must be retried alone; RAISE(ABORT) undoes only the statement -> ROLLBACK TO
 * and the parent commits. Either way the parent is applied exactly once. */
static void test_hint_failure_paths(void) {
    char path[256];
    snprintf(path, sizeof path, "%s/inject.db", dir);
    TransCache *c = open_sqlite(path);
    trans_cache_store_result(c, "auto", "eng", "rollback me", "x", NULL, NULL, NULL);
    trans_cache_store_result(c, "auto", "eng", "abort me", "x", NULL, NULL, NULL);
    char h_rb[65], h_ab[65], sql[512];
    trans_cache_calculate_hash("auto", "eng", "rollback me", h_rb);
    trans_cache_calculate_hash("auto", "eng", "abort me", h_ab);
    sqlite3 *db = raw_open(path);
    snprintf(sql, sizeof sql,
             "CREATE TRIGGER inject_rb BEFORE INSERT ON trans_cache_source_hint WHEN NEW.hash = '%s' "
             "BEGIN SELECT RAISE(ROLLBACK, 'injected rollback'); END;"
             "CREATE TRIGGER inject_ab BEFORE INSERT ON trans_cache_source_hint WHEN NEW.hash = '%s' "
             "BEGIN SELECT RAISE(ABORT, 'injected abort'); END;", h_rb, h_ab);
    CHECK(sqlite3_exec(db, sql, 0, 0, 0) == SQLITE_OK, "inject triggers: %s", sqlite3_errmsg(db));

    SourceHint kor = H("kor", SOURCE_HINT_ORIGIN_SCRIPT, 1, false);
    const char *texts[] = { "rollback me", "abort me" };
    for (int i = 0; i < 2; i++) {
        CacheEntry *e = trans_cache_lookup(c, "auto", "eng", texts[i]);
        CHECK(trans_cache_store_result(c, "auto", "eng", texts[i], "x", e, &kor, NULL) == 0, "%s: parent kept", texts[i]);
        CHECK(e && e->count == 2 && e->hint.origin == SOURCE_HINT_ORIGIN_NONE, "%s: copy count 2, no hint", texts[i]);
        trans_cache_entry_free(e);
    }
    CHECK(raw_int(db, "SELECT count(*) FROM trans_cache WHERE count = 2") == 2, "both parents applied exactly once");
    CHECK(raw_int(db, "SELECT count(*) FROM trans_cache_source_hint") == 0, "no hint written");
    trans_cache_free(c);
    sqlite3_close(db);
}

static int refuse_commit(void *arg) { (void)arg; return 1; }   /* turns COMMIT into ROLLBACK */

/* A failure at COMMIT, after the parent statement succeeded: nothing may be
 * persisted and the caller's copy must stay as it was. */
static void test_commit_failure_keeps_copy(void) {
    char path[256];
    snprintf(path, sizeof path, "%s/commit.db", dir);
    TransCache *c = open_sqlite(path);
    trans_cache_store_result(c, "auto", "eng", "c", "C", NULL, NULL, NULL);
    CacheEntry *e = trans_cache_lookup(c, "auto", "eng", "c");
    SqliteBackendContext *ctx = (SqliteBackendContext *)c->backend_ctx;
    sqlite3_commit_hook(ctx->db, refuse_commit, NULL);
    SourceHint kor = H("kor", SOURCE_HINT_ORIGIN_SCRIPT, 1, false);
    CHECK(trans_cache_store_result(c, "auto", "eng", "c", "C", e, &kor, NULL) == -1, "refused commit -> -1");
    CHECK(e && e->count == 1 && e->hint.origin == SOURCE_HINT_ORIGIN_NONE, "copy untouched after failed commit (count %d)",
          e ? e->count : -1);
    CHECK(trans_cache_store_result(c, "auto", "eng", "c", "CHANGED", e, &kor, NULL) == -1 &&
          strcmp(e->translated_text, "C") == 0, "translation copy untouched after failed commit");
    sqlite3_commit_hook(ctx->db, NULL, NULL);
    sqlite3 *db = raw_open(path);
    CHECK(raw_int(db, "SELECT count FROM trans_cache") == 1, "nothing persisted");
    CHECK(raw_int(db, "SELECT count(*) FROM trans_cache_source_hint") == 0, "no hint persisted");
    sqlite3_close(db);
    trans_cache_entry_free(e);
    trans_cache_free(c);
}

static void create_v0_db(const char *path) {
    sqlite3 *db = raw_open(path);
    const char *sql =
        "CREATE TABLE trans_cache (id INTEGER PRIMARY KEY AUTOINCREMENT, hash TEXT NOT NULL UNIQUE,"
        " from_lang TEXT NOT NULL, to_lang TEXT NOT NULL, source_text TEXT NOT NULL, translated_text TEXT NOT NULL,"
        " count INTEGER DEFAULT 1, last_used INTEGER NOT NULL, created_at INTEGER NOT NULL, CHECK(length(hash) = 64),"
        " CHECK(length(from_lang) = 3), CHECK(length(to_lang) = 3), CHECK(count >= 1));"
        "CREATE INDEX user_idx ON trans_cache(to_lang);"
        "CREATE TABLE audit(n INTEGER);"
        "CREATE TRIGGER user_trg AFTER INSERT ON trans_cache BEGIN INSERT INTO audit VALUES (NEW.id); END;"
        "CREATE VIEW user_view AS SELECT count(*) AS n FROM trans_cache;"
        "INSERT INTO trans_cache(hash,from_lang,to_lang,source_text,translated_text,count,last_used,created_at)"
        " VALUES (printf('%064d', 1),'kor','eng','s','t',3,1,1);"
        "PRAGMA user_version=0;";
    if (sqlite3_exec(db, sql, 0, 0, 0) != SQLITE_OK) { printf("v0 setup: %s\n", sqlite3_errmsg(db)); exit(2); }
    sqlite3_close(db);
}

static void test_migration_and_owned_definitions(void) {
    char path[256];
    snprintf(path, sizeof path, "%s/v0.db", dir);
    create_v0_db(path);
    TransCache *c = open_sqlite(path);
    trans_cache_free(c);
    sqlite3 *db = raw_open(path);
    CHECK(raw_int(db, "PRAGMA user_version") == 2, "v0 migrated to 2");
    CHECK(raw_int(db, "SELECT count FROM trans_cache") == 3, "row preserved");
    CHECK(raw_int(db, "SELECT count(*) FROM sqlite_master WHERE name IN ('user_idx','user_trg','user_view')") == 3,
          "user objects preserved");
    CHECK(raw_int(db, "SELECT count(*) FROM sqlite_master WHERE name IN ('trans_cache_source_hint',"
                      "'trans_cache_source_hint_delete','trans_cache_hint_code','trans_cache_effective_source')") == 4,
          "owned objects after migration");
    char *trg = raw_text(db, "SELECT sql FROM sqlite_master WHERE name='trans_cache_source_hint_delete'");
    char *view = raw_text(db, "SELECT sql FROM sqlite_master WHERE name='trans_cache_effective_source'");

    /* stale owned definitions are replaced on the next start */
    sqlite3_exec(db, "DROP VIEW trans_cache_effective_source; CREATE VIEW trans_cache_effective_source AS SELECT 1 AS x;"
                     "DROP TRIGGER trans_cache_source_hint_delete;"
                     "CREATE TRIGGER trans_cache_source_hint_delete AFTER DELETE ON trans_cache BEGIN SELECT 1; END;", 0, 0, 0);
    sqlite3_close(db);
    c = open_sqlite(path);
    trans_cache_free(c);
    db = raw_open(path);
    char *trg2 = raw_text(db, "SELECT sql FROM sqlite_master WHERE name='trans_cache_source_hint_delete'");
    char *view2 = raw_text(db, "SELECT sql FROM sqlite_master WHERE name='trans_cache_effective_source'");
    CHECK(trg && trg2 && strcmp(trg, trg2) == 0, "trigger definition restored");
    CHECK(view && view2 && strcmp(view, view2) == 0 && strstr(view2, "trans_cache_hint_code"), "view definition restored");
    free(trg); free(view); free(trg2); free(view2);
    sqlite3_close(db);
}

static void test_text_backend(void) {
    char path[256];
    snprintf(path, sizeof path, "%s/cache.jsonl", dir);
    TransCache *c = trans_cache_init_with_backend(CACHE_BACKEND_TEXT, path, NULL);
    SourceHint jpn = H("jpn", SOURCE_HINT_ORIGIN_MODEL, 1, true);
    CHECK(trans_cache_store_result(c, "auto", "eng", "明日", "Tomorrow", NULL, &jpn, NULL) == 0, "text add");
    CHECK(trans_cache_store_result(c, "kor", "eng", "내일", "Tomorrow", NULL, &jpn, NULL) == 0, "text explicit");
    CacheEntry *e = trans_cache_lookup(c, "auto", "eng", "明日");
    CHECK(e && strcmp(e->hint.lang, "jpn") == 0 && e->hint.mixed && e->hint.origin == SOURCE_HINT_ORIGIN_MODEL,
          "text lookup copies the hint");
    CHECK(trans_cache_store_result(c, "auto", "eng", "明日", "Tomorrow", e, NULL, NULL) == 0 && e->count == 2,
          "text update_count synced");
    trans_cache_entry_free(e);
    CHECK(trans_cache_save(c) == 0, "text save");
    trans_cache_free(c);

    FILE *fp = fopen(path, "r");
    char line[2048];
    int with_hint = 0, rows = 0;
    while (fp && fgets(line, sizeof line, fp)) {
        rows++;
        if (strstr(line, "\"source_hint\":\"jpn\"") && strstr(line, "\"hint_origin\":\"model\"")) with_hint++;
    }
    if (fp) fclose(fp);
    CHECK(rows == 2 && with_hint == 1, "JSONL: hint fields on the auto row only (rows %d, hinted %d)", rows, with_hint);

    c = trans_cache_init_with_backend(CACHE_BACKEND_TEXT, path, NULL);
    e = trans_cache_lookup(c, "auto", "eng", "明日");
    CHECK(e && strcmp(e->hint.lang, "jpn") == 0 && e->count == 2, "text hint after reload");
    trans_cache_entry_free(e);
    trans_cache_free(c);

    /* malformed hint fields: the row loads, the hint is dropped */
    const char *bad[] = {
        "\"source_hint\":\"xyz\",\"hint_mixed\":false,\"hint_origin\":\"model\",\"hint_version\":1",
        "\"source_hint\":\"kor\",\"hint_mixed\":false,\"hint_origin\":\"model\",\"hint_version\":\"2\"",
        "\"source_hint\":\"kor\",\"hint_mixed\":1,\"hint_origin\":\"model\",\"hint_version\":1",
        "\"source_hint\":\"kor\",\"hint_mixed\":false,\"hint_origin\":\"guess\",\"hint_version\":1",
        "\"source_hint\":\"kor\",\"hint_mixed\":false,\"hint_origin\":\"model\",\"hint_version\":1.5",
        "\"source_hint\":\"KOR\",\"hint_mixed\":false,\"hint_origin\":\"model\",\"hint_version\":1",
    };
    char hash[65];
    trans_cache_calculate_hash("auto", "eng", "x", hash);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fp = fopen(path, "w");
        fprintf(fp, "{\"id\":1,\"hash\":\"%s\",\"from\":\"auto\",\"to\":\"eng\",\"source\":\"x\",\"target\":\"y\","
                    "\"count\":1,\"last_used\":1,\"created_at\":1,%s}\n", hash, bad[i]);
        fclose(fp);
        c = trans_cache_init_with_backend(CACHE_BACKEND_TEXT, path, NULL);
        e = trans_cache_lookup(c, "auto", "eng", "x");
        CHECK(e && e->hint.origin == SOURCE_HINT_ORIGIN_NONE, "malformed hint %zu dropped, row kept", i);
        trans_cache_entry_free(e);
        trans_cache_free(c);
    }
}

int main(void) {
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    test_new_db_and_basic_roundtrip();
    test_priority_and_invalid();
    test_vanished_deadline_busy();
    test_full_aborts_then_parent_retry();
    test_hint_failure_paths();
    test_commit_failure_keeps_copy();
    test_migration_and_owned_definitions();
    test_text_backend();
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) printf("warning: could not remove %s\n", dir);
    printf("source_hint storage tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
