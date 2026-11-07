/* PLAN_FAULT_INJECTION.md F4, F5, F6 and Q1: failures inside the result + hint
 * store of both cache backends.
 *
 *   make test-fault-store          (TEST_SANITIZERS='-fsanitize=address,undefined ...' for leaks)
 *
 * The two backend sources are included with counting malloc/calloc/realloc/strdup/
 * strndup wrappers (F5: allocations made in those files only). SQLite gets a forwarding
 * allocator through SQLITE_CONFIG_MALLOC (installed before the first open, lookaside
 * off) that can fail its k-th allocation (M2b), and an authorizer that refuses the n-th
 * SAVEPOINT / RELEASE / ROLLBACK TO / BEGIN / COMMIT / ROLLBACK (M2). F6 calls the
 * backend's store_result directly with an expired and with a cancelled control.
 *
 * Fixtures (source text in from=auto): a new row, an existing row whose count goes up,
 * an existing row whose translation changes, a row that vanished after the caller's
 * lookup; without a hint, with a new hint, and replacing an older hint (the new hint
 * differs from the old one in language, origin and mixed). After every store, the committed state - read through a
 * separate SQLite connection, or a fresh lookup for the text backend - must match the
 * return value: 0 = the parent applied exactly once and the hint written or (if the
 * hint step failed) the old hint kept; -1 = nothing changed. The caller's copy changes
 * only with a commit (translation, count, last_used, hint), and no hint row is ever
 * left without its parent. The connection ends outside a transaction, unless ROLLBACK
 * itself kept failing ("stuck"); then the next store, once the cause is gone, ends that
 * transaction and is skipped (its snapshot may be stale - Q1), another connection can
 * write again, and the store after that works.
 */
#define _GNU_SOURCE
#include <cjson/cJSON.h>
#include <limits.h>
#include <regex.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "cache_backend_sqlite.h"
#include "cache_backend_text.h"
#include "trans_cache.h"
#include "utils.h"

/* ---- counting C allocator for the two included backends (F5) ---- */
static int tb_armed;
static long tb_fail_at = -1, tb_count;
static int tb_persist;
static int tb_should_fail(void) {
    if (!tb_armed) return 0;
    long n = ++tb_count;
    if (tb_fail_at <= 0) return 0;
    return tb_persist ? n >= tb_fail_at : n == tb_fail_at;
}
__attribute__((unused)) static void *tb_malloc(size_t n) { return tb_should_fail() ? NULL : malloc(n); }
__attribute__((unused)) static void *tb_calloc(size_t a, size_t b) { return tb_should_fail() ? NULL : calloc(a, b); }
__attribute__((unused)) static void *tb_realloc(void *p, size_t n) { return tb_should_fail() ? NULL : realloc(p, n); }
__attribute__((unused)) static char *tb_strdup(const char *s) { return tb_should_fail() ? NULL : strdup(s); }
__attribute__((unused)) static char *tb_strndup(const char *s, size_t n) { return tb_should_fail() ? NULL : strndup(s, n); }
#define malloc(n) tb_malloc(n)
#define calloc(a, b) tb_calloc(a, b)
#define realloc(p, n) tb_realloc(p, n)
#define strdup(s) tb_strdup(s)
#define strndup(s, n) tb_strndup(s, n)
#include "../src/cache_backend_text.c"
#include "../src/cache_backend_sqlite.c"
#undef malloc
#undef calloc
#undef realloc
#undef strdup
#undef strndup

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- SQLite allocator (M2b) ---- */
static sqlite3_mem_methods g_def;
static int sq_armed, sq_persist;
static long sq_fail_at = -1, sq_count;
static int sq_should_fail(void) {
    if (!sq_armed) return 0;
    long n = ++sq_count;
    if (sq_fail_at <= 0) return 0;
    return sq_persist ? n >= sq_fail_at : n == sq_fail_at;
}
static void *sq_malloc(int n) { return sq_should_fail() ? NULL : g_def.xMalloc(n); }
static void sq_free(void *p) { g_def.xFree(p); }
static void *sq_realloc(void *p, int n) { return sq_should_fail() ? NULL : g_def.xRealloc(p, n); }
static int sq_size(void *p) { return g_def.xSize(p); }
static int sq_roundup(int n) { return g_def.xRoundup(n); }
static int sq_init(void *a) { return g_def.xInit(a); }
static void sq_shutdown(void *a) { g_def.xShutdown(a); }

/* ---- authorizer (M2) ---- */
typedef struct { int action; const char *arg; int nth; int seen; } Deny;   /* nth 0 = always */
static Deny g_deny[4];
static int g_ndeny;
static int authorizer(void *u, int action, const char *a1, const char *a2, const char *d, const char *t) {
    (void)u; (void)a2; (void)d; (void)t;
    int refuse = 0;                    /* count every matching rule first, then decide */
    for (int i = 0; i < g_ndeny; i++) {
        Deny *r = &g_deny[i];
        if (action == r->action && a1 && strcmp(a1, r->arg) == 0) {
            r->seen++;
            if (r->nth == 0 || r->seen == r->nth) refuse = 1;
        }
    }
    return refuse ? SQLITE_DENY : SQLITE_OK;
}
static void deny_reset(void) { g_ndeny = 0; }
static void deny(int action, const char *arg, int nth) { g_deny[g_ndeny++] = (Deny){ action, arg, nth, 0 }; }

/* ---- fixtures ---- */
enum { BK_SQLITE, BK_TEXT };
enum { P_INSERT, P_COUNT, P_TRANS, P_VANISH };
enum { H_NONE, H_NEW, H_OLD };
static const char *PNAME[] = { "new row", "count up", "translation change", "row vanished after the lookup" };
static const char *HNAME[] = { "no hint", "new hint", "replaces old hint" };
static const char *TEXT = "原文 テキスト";
static char g_dir[] = "/tmp/fault-store-XXXXXX";
static int g_serial;

typedef struct {
    int exists; char trans[32]; int count; long long last_used;     /* last_used: not compared by same_state */
    char hint_lang[4]; char hint_origin; int hint_mixed, hint_version;
    int orphans;                   /* hint rows without a parent (sqlite) - must stay 0 */
} State;

typedef struct {
    int backend, parent, hint;
    char path[512];
    TransCache *c;
    CacheEntry *cached;        /* the caller's copy (NULL for a new row) */
    char hash[65];
    State before, want, want_hint_failed;
    State copy_before;         /* the caller's copy as handed in */
    const char *translated;
    SourceHint new_hint;
} Fx;

static SourceHint mk_hint(const char *lang, char origin, bool mixed) {
    SourceHint h = { .mixed = mixed, .origin = origin, .version = 1 };
    memcpy(h.lang, lang, 4);
    return h;
}

static State state_of_entry(const CacheEntry *e) {
    State s = { 0 };
    if (!e) return s;
    s.exists = 1;
    snprintf(s.trans, sizeof s.trans, "%s", e->translated_text);
    s.count = e->count;
    s.last_used = (long long)e->last_used;
    if (source_hint_valid(&e->hint)) {
        memcpy(s.hint_lang, e->hint.lang, 4);
        s.hint_origin = e->hint.origin;
        s.hint_mixed = e->hint.mixed;
        s.hint_version = e->hint.version;
    }
    return s;
}

/* The committed state: a fresh lookup for the text backend (its "committed" state is
 * the live memory), a second connection for SQLite. Any SQL error stops the run. */
static State read_state(Fx *f) {
    if (f->backend == BK_TEXT) {
        CacheEntry *e = trans_cache_lookup(f->c, "auto", "eng", TEXT);
        State s = state_of_entry(e);
        trans_cache_entry_free(e);
        return s;
    }
    State s = { 0 };
    sqlite3 *db = NULL;
    if (sqlite3_open(f->path, &db) != SQLITE_OK) { printf("raw open failed\n"); exit(2); }
    sqlite3_busy_timeout(db, 2000);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT p.translated_text, p.count, p.last_used, h.source_hint, h.origin, h.mixed, "
                               "h.hint_version FROM trans_cache p LEFT JOIN trans_cache_source_hint h ON h.hash = p.hash "
                               "WHERE p.hash = ?", -1, &st, NULL) != SQLITE_OK) {
        printf("read_state: %s\n", sqlite3_errmsg(db)); exit(2);
    }
    sqlite3_bind_text(st, 1, f->hash, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        s.exists = 1;
        snprintf(s.trans, sizeof s.trans, "%s", (const char *)sqlite3_column_text(st, 0));
        s.count = sqlite3_column_int(st, 1);
        s.last_used = sqlite3_column_int64(st, 2);
        if (sqlite3_column_text(st, 3)) {
            snprintf(s.hint_lang, sizeof s.hint_lang, "%s", (const char *)sqlite3_column_text(st, 3));
            s.hint_origin = strcmp((const char *)sqlite3_column_text(st, 4), "model") == 0 ? SOURCE_HINT_ORIGIN_MODEL
                                                                                         : SOURCE_HINT_ORIGIN_SCRIPT;
            s.hint_mixed = sqlite3_column_int(st, 5);
            s.hint_version = sqlite3_column_int(st, 6);
        }
        rc = sqlite3_step(st);
    }
    if (rc != SQLITE_DONE) { printf("read_state step: %s\n", sqlite3_errmsg(db)); exit(2); }
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM trans_cache_source_hint h WHERE NOT EXISTS "
                               "(SELECT 1 FROM trans_cache p WHERE p.hash = h.hash)", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_step(st) != SQLITE_ROW) {
        printf("orphans: %s\n", sqlite3_errmsg(db)); exit(2);
    }
    s.orphans = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return s;
}

static int same_state(const State *a, const State *b) {
    return a->exists == b->exists && (!a->exists || (strcmp(a->trans, b->trans) == 0 && a->count == b->count &&
           strcmp(a->hint_lang, b->hint_lang) == 0 && a->hint_origin == b->hint_origin &&
           a->hint_mixed == b->hint_mixed && a->hint_version == b->hint_version));
}

static void show(const State *s, char *buf, size_t n) {
    if (!s->exists) snprintf(buf, n, "(no row)");
    else snprintf(buf, n, "%s x%d hint %s/%c/m%d/v%d", s->trans, s->count, s->hint_lang[0] ? s->hint_lang : "-",
                  s->hint_origin ? s->hint_origin : '-', s->hint_mixed, s->hint_version);
}

static void fx_open(Fx *f, int backend, int parent, int hint, int fill) {
    memset(f, 0, sizeof *f);
    f->backend = backend; f->parent = parent; f->hint = hint;
    snprintf(f->path, sizeof f->path, "%s/f%d.%s", g_dir, ++g_serial, backend == BK_SQLITE ? "db" : "jsonl");
    f->c = trans_cache_init_with_backend(backend == BK_SQLITE ? CACHE_BACKEND_SQLITE : CACHE_BACKEND_TEXT, f->path, NULL);
    if (!f->c) { printf("cache init failed\n"); exit(2); }
    trans_cache_calculate_hash("auto", "eng", TEXT, f->hash);
    for (int i = 0; i < fill; i++) {           /* text: reach the capacity boundary */
        char t[32];
        snprintf(t, sizeof t, "filler %d", i);
        trans_cache_store_result(f->c, "kor", "eng", t, "x", NULL, NULL, NULL);
    }
    if (parent != P_INSERT) {
        SourceHint old = mk_hint("jpn", SOURCE_HINT_ORIGIN_SCRIPT, false);
        trans_cache_store_result(f->c, "auto", "eng", TEXT, "A", NULL, hint == H_OLD ? &old : NULL, NULL);
        f->cached = trans_cache_lookup(f->c, "auto", "eng", TEXT);
        if (!f->cached) { printf("fixture lookup failed\n"); exit(2); }
    }
    if (parent == P_VANISH) {                     /* the row goes away after the caller's lookup */
        if (backend == BK_SQLITE) {
            sqlite3 *db = NULL;
            sqlite3_open(f->path, &db);
            char sql[200];
            snprintf(sql, sizeof sql, "DELETE FROM trans_cache WHERE hash = '%s'", f->hash);
            if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK || sqlite3_changes(db) != 1) {
                printf("vanish: %s\n", sqlite3_errmsg(db)); exit(2);
            }
            sqlite3_close(db);
        } else {
            TextBackendContext *ctx = f->c->backend_ctx;
            size_t i = 0;
            while (i < ctx->size && strcmp(ctx->entries[i]->hash, f->hash) != 0) i++;
            if (i == ctx->size) { printf("vanish: not found\n"); exit(2); }
            CacheEntry *gone = ctx->entries[i];
            ctx->entries[i] = ctx->entries[--ctx->size];
            free(gone->source_text); free(gone->translated_text); free(gone);
        }
    }
    f->translated = parent == P_TRANS ? "B" : "A";
    f->new_hint = mk_hint("kor", SOURCE_HINT_ORIGIN_MODEL, true);   /* differs from the old one in every field */
    f->before = read_state(f);
    f->copy_before = state_of_entry(f->cached);
    State w = { 0 };
    w.exists = 1;
    w.count = parent == P_COUNT ? 2 : 1;
    snprintf(w.trans, sizeof w.trans, "%s", f->translated);
    State wf = w;                                  /* the hint step failed: old hint (if any) kept */
    memcpy(wf.hint_lang, f->before.hint_lang, 4);
    wf.hint_origin = f->before.hint_origin;
    wf.hint_mixed = f->before.hint_mixed;
    wf.hint_version = f->before.hint_version;
    if (hint != H_NONE) {
        memcpy(w.hint_lang, "kor", 4); w.hint_origin = SOURCE_HINT_ORIGIN_MODEL; w.hint_mixed = 1; w.hint_version = 1;
    } else {
        w = wf;
    }
    f->want = w;
    f->want_hint_failed = wf;
}

static void fx_close(Fx *f) {
    trans_cache_entry_free(f->cached);
    trans_cache_free(f->c);
    unlink(f->path);
    char extra[600];
    snprintf(extra, sizeof extra, "%s-wal", f->path); unlink(extra);
    snprintf(extra, sizeof extra, "%s-shm", f->path); unlink(extra);
}

static sqlite3 *fx_db(Fx *f) { return ((SqliteBackendContext *)f->c->backend_ctx)->db; }

static int store(Fx *f, const TranslationControl *ctl, int direct) {
    const SourceHint *h = f->hint == H_NONE ? NULL : &f->new_hint;
    if (direct)
        return f->c->ops->store_result(f->c->backend_ctx, "auto", "eng", TEXT, f->translated, f->cached, h, ctl);
    return trans_cache_store_result(f->c, "auto", "eng", TEXT, f->translated, f->cached, h, ctl);
}

/* What every store must satisfy. `hint_may_fail`: the injected fault may hit the hint
 * step only, so the parent with the old hint is also a correct 0. Returns 1 when the
 * connection was left inside a transaction. */
static int verify(Fx *f, int rc, int hint_may_fail, const char *what, int want_rc) {
    char a[96], b[96];
    State s = read_state(f);
    const State *want0 = &f->want;
    if (rc == 0 && hint_may_fail && same_state(&s, &f->want_hint_failed)) want0 = &f->want_hint_failed;
    CHECK(rc == 0 || rc == -1, "%s: store returned %d (only 0 or -1)", what, rc);
    if (want_rc != 99) CHECK(rc == want_rc, "%s: store returned %d, want %d", what, rc, want_rc);
    CHECK(s.orphans == 0, "%s: %d hint row(s) without a parent", what, s.orphans);
    if (rc == 0) {
        show(&s, a, sizeof a); show(want0, b, sizeof b);
        CHECK(same_state(&s, want0), "%s: returned 0, committed %s, want %s", what, a, b);
    } else {
        show(&s, a, sizeof a); show(&f->before, b, sizeof b);
        CHECK(same_state(&s, &f->before), "%s: returned %d, committed %s, want unchanged %s", what, rc, a, b);
    }
    if (f->cached) {                  /* the copy follows a commit, and only a commit */
        State c = state_of_entry(f->cached);
        const State *ref = rc == 0 ? &s : &f->copy_before;
        show(&c, a, sizeof a); show(ref, b, sizeof b);
        CHECK(same_state(&c, ref) && c.last_used == ref->last_used, "%s: caller copy %s (last_used %lld), want %s (%lld)",
              what, a, c.last_used, b, ref->last_used);
    }
    return f->backend == BK_SQLITE && !sqlite3_get_autocommit(fx_db(f));
}

/* After a stuck connection, once the cause is gone (Q1): the next store ends the open
 * transaction and is itself skipped (-1, its snapshot may be stale), another connection
 * can take the write lock again, and the store after that works. */
static void verify_recovery(Fx *f, const char *what) {
    deny_reset(); sq_armed = 0; tb_armed = 0;
    State committed = read_state(f);
    int rc = trans_cache_store_result(f->c, "kor", "eng", "recovery", "R", NULL, NULL, NULL);
    CHECK(rc == -1, "%s: the store that ends the leftover transaction returned %d, want -1 (skipped)", what, rc);
    CHECK(sqlite3_get_autocommit(fx_db(f)), "%s: still inside a transaction after the next store (not recovered)", what);
    State now = read_state(f);
    CHECK(same_state(&now, &committed), "%s: the recovery changed the committed state", what);
    sqlite3 *db = NULL;
    sqlite3_open(f->path, &db);
    int brc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    CHECK(brc == SQLITE_OK, "%s: another connection cannot write (%s)", what, sqlite3_errstr(brc));
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(db);
    rc = trans_cache_store_result(f->c, "kor", "eng", "recovery", "R", NULL, NULL, NULL);
    CHECK(rc == 0, "%s: the store after the recovery returned %d", what, rc);
}

/* Codex's counterexample to the first Q1 fix: while stuck, a lookup on the same
 * connection copies the uncommitted B; the store that recovers must not build on it
 * (it committed A x2 and left the copy at B x2). */
static void stale_snapshot(void) {
    int before = failures;
    Fx f;
    fx_open(&f, BK_SQLITE, P_TRANS, H_NONE, 0);
    sqlite3_set_authorizer(fx_db(&f), authorizer, NULL);
    deny_reset();
    deny(SQLITE_TRANSACTION, "COMMIT", 0);
    deny(SQLITE_TRANSACTION, "ROLLBACK", 0);
    CHECK(store(&f, NULL, 0) == -1 && !sqlite3_get_autocommit(fx_db(&f)), "stale: setup did not get stuck");
    deny_reset();
    trans_cache_entry_free(f.cached);
    f.cached = trans_cache_lookup(f.c, "auto", "eng", TEXT);
    CHECK(f.cached && strcmp(f.cached->translated_text, "B") == 0, "stale: the stuck connection should show B");
    f.copy_before = state_of_entry(f.cached);
    int rc = store(&f, NULL, 0);
    verify(&f, rc, 0, "stale snapshot, recovering store", -1);
    CHECK(sqlite3_get_autocommit(fx_db(&f)), "stale: still inside a transaction");
    trans_cache_entry_free(f.cached);
    f.cached = trans_cache_lookup(f.c, "auto", "eng", TEXT);
    CHECK(f.cached && strcmp(f.cached->translated_text, "A") == 0 && f.cached->count == 1,
          "stale: after the recovery the lookup shows the committed A x1");
    f.copy_before = state_of_entry(f.cached);
    rc = store(&f, NULL, 0);
    verify(&f, rc, 0, "stale snapshot, next store from a fresh lookup", 0);
    sqlite3_set_authorizer(fx_db(&f), NULL, NULL);
    fx_close(&f);
    printf("  %s a stale copy from a stuck connection is not built on (store skipped, then a fresh lookup works)\n",
           failures == before ? "ok  " : "FAIL");
}

static void add_abort_trigger(Fx *f) {
    sqlite3 *db = NULL;
    sqlite3_open(f->path, &db);
    int rc = sqlite3_exec(db, "CREATE TRIGGER inject_ab BEFORE INSERT ON trans_cache_source_hint "
                              "BEGIN SELECT RAISE(ABORT, 'injected abort'); END;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { printf("trigger: %s\n", sqlite3_errmsg(db)); exit(2); }
    sqlite3_close(db);
}

/* ---- M2: one SQL statement refused ---- */
typedef struct {
    const char *name;
    int needs_hint, abort_hint;          /* hint fixtures only / hint insert raises ABORT */
    Deny rules[3];
    int nrules;
    int want_rc, hint_fails, stuck;      /* stuck: ROLLBACK refused for good */
} Scenario;

static void scenarios(void) {
    const Scenario sc[] = {
        { "SAVEPOINT refused", 1, 0, { { SQLITE_SAVEPOINT, "BEGIN", 1, 0 } }, 1, 0, 1, 0 },
        { "success-side RELEASE refused (result ignored)", 1, 0, { { SQLITE_SAVEPOINT, "RELEASE", 1, 0 } }, 1, 0, 0, 0 },
        { "hint fails, ROLLBACK TO refused", 1, 1, { { SQLITE_SAVEPOINT, "ROLLBACK", 1, 0 } }, 1, 0, 1, 0 },
        { "hint fails, RELEASE refused", 1, 1, { { SQLITE_SAVEPOINT, "RELEASE", 1, 0 } }, 1, 0, 1, 0 },
        { "BEGIN refused", 0, 0, { { SQLITE_TRANSACTION, "BEGIN", 1, 0 } }, 1, -1, 0, 0 },
        { "COMMIT refused", 0, 0, { { SQLITE_TRANSACTION, "COMMIT", 1, 0 } }, 1, -1, 0, 0 },
        { "hint fails, ROLLBACK TO refused, retry's BEGIN refused", 1, 1,
          { { SQLITE_SAVEPOINT, "ROLLBACK", 1, 0 }, { SQLITE_TRANSACTION, "BEGIN", 2, 0 } }, 2, -1, 0, 0 },
        { "COMMIT refused, ROLLBACK refused twice then allowed", 0, 0,
          { { SQLITE_TRANSACTION, "COMMIT", 1, 0 }, { SQLITE_TRANSACTION, "ROLLBACK", 1, 0 },
            { SQLITE_TRANSACTION, "ROLLBACK", 2, 0 } }, 3, -1, 0, 0 },
        { "COMMIT refused, ROLLBACK always refused (stuck)", 0, 0,
          { { SQLITE_TRANSACTION, "COMMIT", 1, 0 }, { SQLITE_TRANSACTION, "ROLLBACK", 0, 0 } }, 2, -1, 0, 1 },
    };
    for (size_t i = 0; i < sizeof sc / sizeof sc[0]; i++) {
        int before = failures, runs = 0;
        for (int p = P_INSERT; p <= P_VANISH; p++) {
            for (int h = H_NONE; h <= H_OLD; h++) {
                if (h == H_OLD && p == P_INSERT) continue;
                if (sc[i].needs_hint && h == H_NONE) continue;
                Fx f;
                fx_open(&f, BK_SQLITE, p, h, 0);
                if (sc[i].abort_hint) add_abort_trigger(&f);
                sqlite3_set_authorizer(fx_db(&f), authorizer, NULL);
                deny_reset();
                for (int r = 0; r < sc[i].nrules; r++) deny(sc[i].rules[r].action, sc[i].rules[r].arg, sc[i].rules[r].nth);
                char what[200];
                snprintf(what, sizeof what, "%s [%s, %s]", sc[i].name, PNAME[p], HNAME[h]);
                int rc = store(&f, NULL, 0);
                for (int r = 0; r < g_ndeny; r++)       /* the scenario really reached its statement */
                    CHECK(g_deny[r].seen >= (g_deny[r].nth ? g_deny[r].nth : 1), "%s: rule %d never fired (seen %d)",
                          what, r, g_deny[r].seen);
                int stuck = verify(&f, rc, sc[i].hint_fails, what, sc[i].want_rc);
                /* exact outcome: hint written or (hint_fails) old hint kept */
                if (rc == 0) {
                    State s = read_state(&f);
                    CHECK(same_state(&s, sc[i].hint_fails ? &f.want_hint_failed : &f.want),
                          "%s: hint outcome not the expected one", what);
                }
                CHECK(stuck == sc[i].stuck, "%s: connection %s a transaction", what, stuck ? "left inside" : "outside");
                if (stuck) verify_recovery(&f, what);
                deny_reset();
                sqlite3_set_authorizer(fx_db(&f), NULL, NULL);
                fx_close(&f);
                runs++;
            }
        }
        printf("  %s %s (%d fixtures)\n", failures == before ? "ok  " : "FAIL", sc[i].name, runs);
    }
}

/* ---- M2b and F5: allocation sweeps ---- */
static void alloc_sweeps(void) {
    for (int which = 0; which < 3; which++) {          /* 0: SQLite's own, 1: C in sqlite backend, 2: C in text */
        int backend = which == 2 ? BK_TEXT : BK_SQLITE;
        const char *label = which == 0 ? "SQLite allocations (sqlite backend)" :
                            which == 1 ? "C allocations (sqlite backend)" : "C allocations (text backend)";
        int before = failures;
        long calls = 0, zeros = 0, minus = 0, stuck_n = 0, maxn = 0;
        for (int p = P_INSERT; p <= P_VANISH; p++) {
            for (int h = H_NONE; h <= H_OLD; h++) {
                if (h == H_OLD && p == P_INSERT) continue;
                for (int fill = 0; fill <= (backend == BK_TEXT && p == P_INSERT ? 1 : 0); fill++) {
                    int nfill = fill ? INITIAL_CAPACITY : 0;   /* the new row needs the capacity realloc */
                    Fx f;
                    fx_open(&f, backend, p, h, nfill);
                    /* baseline count on an equivalent fixture */
                    long n;
                    if (which == 0) { sq_fail_at = -1; sq_count = 0; sq_armed = 1; } else { tb_fail_at = -1; tb_count = 0; tb_armed = 1; }
                    int rc0 = store(&f, NULL, 0);
                    sq_armed = tb_armed = 0;
                    n = which == 0 ? sq_count : tb_count;
                    char what[200];
                    snprintf(what, sizeof what, "%s baseline [%s, %s%s]", label, PNAME[p], HNAME[h], fill ? ", at capacity" : "");
                    verify(&f, rc0, 0, what, 0);
                    fx_close(&f);
                    if (n > maxn) maxn = n;
                    for (int persist = 0; persist <= 1; persist++) {
                        for (long k = 1; k <= n; k++) {
                            fx_open(&f, backend, p, h, nfill);
                            if (which == 0) { sq_fail_at = k; sq_persist = persist; sq_count = 0; sq_armed = 1; }
                            else { tb_fail_at = k; tb_persist = persist; tb_count = 0; tb_armed = 1; }
                            int rc = store(&f, NULL, 0);
                            sq_armed = tb_armed = 0;
                            long seen = which == 0 ? sq_count : tb_count;
                            snprintf(what, sizeof what, "%s %s k=%ld [%s, %s%s]", label, persist ? "persistent" : "single",
                                     k, PNAME[p], HNAME[h], fill ? ", at capacity" : "");
                            CHECK(seen >= k, "%s: only %ld allocations, the failure was never injected", what, seen);
                            /* only SQLite's own allocations can fail the hint step alone */
                            int stuck = verify(&f, rc, which == 0, what, 99);
                            calls++;
                            if (rc == 0) zeros++; else minus++;
                            if (stuck) { stuck_n++; verify_recovery(&f, what); }
                            else if (backend == BK_SQLITE) {
                                /* the connection still works */
                                CHECK(trans_cache_store_result(f.c, "kor", "eng", "after", "X", NULL, NULL, NULL) == 0,
                                      "%s: the next store failed", what);
                            }
                            fx_close(&f);
                        }
                    }
                }
            }
        }
        printf("  %s %s: up to %ld per store, %ld failing stores -> %ld returned 0, %ld returned -1, %ld left the "
               "connection in a transaction (each checked: the next store must recover)\n", failures == before ? "ok  " : "FAIL",
               label, maxn, calls, zeros, minus, stuck_n);
    }
}

/* ---- F6: the backend's own deadline / cancellation check ---- */
static void backend_deadline(void) {
    for (int backend = BK_SQLITE; backend <= BK_TEXT; backend++) {
        int before = failures, runs = 0;
        for (int kind = 0; kind < 2; kind++) {           /* 0 expired, 1 cancelled */
            atomic_bool cancelled = true;
            TranslationControl ctl;
            clock_gettime(CLOCK_MONOTONIC, &ctl.deadline);
            if (kind == 0) { ctl.deadline.tv_sec -= 1; ctl.cancelled = NULL; }
            else { ctl.deadline.tv_sec += 60; ctl.cancelled = &cancelled; }
            for (int p = P_INSERT; p <= P_VANISH; p++) {
                for (int h = H_NONE; h <= H_OLD; h++) {
                    if (h == H_OLD && p == P_INSERT) continue;
                    Fx f;
                    fx_open(&f, backend, p, h, 0);
                    char what[200];
                    snprintf(what, sizeof what, "%s backend, %s, called directly [%s, %s]",
                             backend == BK_SQLITE ? "sqlite" : "text", kind ? "cancelled" : "deadline passed", PNAME[p], HNAME[h]);
                    int rc = store(&f, &ctl, 1);
                    int stuck = verify(&f, rc, 0, what, -1);
                    CHECK(!stuck, "%s: left inside a transaction", what);
                    if (backend == BK_SQLITE)
                        CHECK(trans_cache_store_result(f.c, "kor", "eng", "after", "X", NULL, NULL, NULL) == 0,
                              "%s: the next store failed", what);
                    fx_close(&f);
                    runs++;
                }
            }
        }
        printf("  %s %s backend's own deadline/cancellation check (%d stores, called past the wrapper)\n",
               failures == before ? "ok  " : "FAIL", backend == BK_SQLITE ? "sqlite" : "text", runs);
    }
}

int main(void) {
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 2; }
    sqlite3_config(SQLITE_CONFIG_GETMALLOC, &g_def);
    sqlite3_mem_methods m = g_def;
    m.xMalloc = sq_malloc; m.xFree = sq_free; m.xRealloc = sq_realloc;
    m.xSize = sq_size; m.xRoundup = sq_roundup; m.xInit = sq_init; m.xShutdown = sq_shutdown;
    int rc1 = sqlite3_config(SQLITE_CONFIG_MALLOC, &m);
    int rc2 = sqlite3_config(SQLITE_CONFIG_LOOKASIDE, 0, 0);    /* every allocation through xMalloc */
    CHECK(rc1 == SQLITE_OK && rc2 == SQLITE_OK, "SQLite allocator not installed (%d, %d)", rc1, rc2);
    scenarios();
    stale_snapshot();
    backend_deadline();
    alloc_sweeps();
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
    if (system(cmd) != 0) printf("  (could not remove %s)\n", g_dir);
    printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
