/**
 * SQLite backend implementation for translation cache.
 * Uses prepared statements for high performance and SQL injection protection.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <regex.h>
#include <limits.h>
#include "cache_backend_sqlite.h"
#include "trans_cache.h"
#include "utils.h"

/* Forward declarations of backend operations */
static CacheEntry* sqlite_backend_lookup(void *ctx, const char *from_lang,
                                          const char *to_lang, const char *text);
static int sqlite_backend_add(void *ctx, const char *from_lang, const char *to_lang,
                               const char *source_text, const char *translated_text);
static int sqlite_backend_update_count(void *ctx, CacheEntry *entry);
static int sqlite_backend_update_translation(void *ctx, CacheEntry *entry,
                                              const char *new_translation);
static int sqlite_backend_save(void *ctx);
static int sqlite_backend_cleanup(void *ctx, int days_threshold);
static void sqlite_backend_stats(void *ctx, size_t *total_entries,
                                  size_t *active_entries, size_t *expired_entries,
                                  int cache_threshold, int days_threshold);
static void sqlite_backend_free(void *ctx);
static int sqlite_backend_store_result(void *ctx, const char *from_lang, const char *to_lang,
                                       const char *source_text, const char *translated_text,
                                       CacheEntry *cached, const SourceHint *hint,
                                       const TranslationControl *control);

/* SQL schema and statements */
static const char *SQL_CREATE_TABLE =
    "CREATE TABLE IF NOT EXISTS trans_cache ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  hash TEXT NOT NULL UNIQUE,"
    "  from_lang TEXT NOT NULL,"
    "  to_lang TEXT NOT NULL,"
    "  source_text TEXT NOT NULL,"
    "  translated_text TEXT NOT NULL,"
    "  count INTEGER DEFAULT 1,"
    "  last_used INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  CHECK(length(hash) = 64),"
    "  CHECK(length(from_lang) = 3 OR from_lang = 'auto'),"
    "  CHECK(length(to_lang) = 3),"
    "  CHECK(count >= 1)"
    ");";

static const char *SQL_CREATE_INDEX_HASH =
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_hash ON trans_cache(hash);";

static const char *SQL_CREATE_INDEX_LANG_PAIR =
    "CREATE INDEX IF NOT EXISTS idx_lang_pair ON trans_cache(from_lang, to_lang);";

static const char *SQL_CREATE_INDEX_LAST_USED =
    "CREATE INDEX IF NOT EXISTS idx_last_used ON trans_cache(last_used);";

static const char *SQL_CREATE_INDEX_COUNT =
    "CREATE INDEX IF NOT EXISTS idx_count ON trans_cache(count DESC);";

static const char *SQL_CREATE_INDEX_LANG_HASH =
    "CREATE INDEX IF NOT EXISTS idx_lang_hash ON trans_cache(from_lang, to_lang, hash);";

static int schema_exec(sqlite3 *db, const char *sql) {
    char *message = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (rc != SQLITE_OK) LOG_INFO("Cache schema error: %s", message ? message : sqlite3_errmsg(db));
    sqlite3_free(message);
    return rc == SQLITE_OK ? 0 : -1;
}

static int schema_integer(sqlite3 *db, const char *sql, sqlite3_int64 *value) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) *value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? 0 : -1;
}

/* Only migrate the known column layout. Preserve the original table body so
 * constraints from the standalone SQL file are not lost or newly imposed. */
static int known_cache_columns(sqlite3 *db) {
    const char *names[] = {"id", "hash", "from_lang", "to_lang", "source_text",
                          "translated_text", "count", "last_used", "created_at"};
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA table_xinfo(trans_cache)", -1, &stmt, NULL) != SQLITE_OK) return 0;
    size_t i = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        if (i >= sizeof(names) / sizeof(names[0]) || !name || strcmp(name, names[i]) != 0) break;
        i++;
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && i == sizeof(names) / sizeof(names[0]);
}

static int source_constraint(const char *ddl, bool automatic, regmatch_t *match) {
    const char *old = "CHECK[[:space:]]*\\([[:space:]]*length[[:space:]]*\\([[:space:]]*from_lang[[:space:]]*\\)[[:space:]]*=[[:space:]]*3[[:space:]]*\\)";
    const char *current = "CHECK[[:space:]]*\\([[:space:]]*length[[:space:]]*\\([[:space:]]*from_lang[[:space:]]*\\)[[:space:]]*=[[:space:]]*3[[:space:]]+OR[[:space:]]+from_lang[[:space:]]*=[[:space:]]*'auto'[[:space:]]*\\)";
    regex_t regex;
    if (regcomp(&regex, automatic ? current : old, REG_EXTENDED | REG_ICASE) != 0) return 0;
    int found = regexec(&regex, ddl, 1, match, 0) == 0;
    regfree(&regex);
    return found;
}

/* Runs within the startup transaction, before any prepared cache statements.
 * Save custom indexes/triggers and the high-water sequence, including deleted
 * IDs. legacy_alter_table keeps existing views valid while swapping tables. */
static int migrate_source_constraint(sqlite3 *db, const char *ddl, regmatch_t match) {
    const char *body = strchr(ddl, '(');
    if (!body || body > ddl + match.rm_so) return -1;
    sqlite3_int64 sequence = 0;
    if (schema_integer(db, "SELECT coalesce(max(seq),0) FROM sqlite_sequence WHERE name='trans_cache'", &sequence) != 0) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master WHERE tbl_name='trans_cache' "
                          "AND type IN ('index','trigger') AND sql IS NOT NULL", -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_str *builder = sqlite3_str_new(db);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_str_appendf(builder, "%s;\n", sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    int builder_error = sqlite3_str_errcode(builder);
    char *objects = sqlite3_str_finish(builder);
    if (rc != SQLITE_DONE || builder_error != SQLITE_OK) { sqlite3_free(objects); return -1; }

    char *create = sqlite3_mprintf("CREATE TABLE trans_cache_auto_new %.*s"
        "CHECK(length(from_lang) = 3 OR from_lang = 'auto')%s",
        (int)(ddl + match.rm_so - body), body, ddl + match.rm_eo);
    if (!create) { sqlite3_free(objects); return -1; }
    int result = schema_exec(db, create);
    sqlite3_free(create);
    if (result == 0) result = schema_exec(db,
        "INSERT INTO trans_cache_auto_new (id,hash,from_lang,to_lang,source_text,translated_text,count,last_used,created_at) "
        "SELECT id,hash,from_lang,to_lang,source_text,translated_text,count,last_used,created_at FROM trans_cache;"
        "DROP TABLE trans_cache;"
        "ALTER TABLE trans_cache_auto_new RENAME TO trans_cache;");
    if (result == 0 && objects) result = schema_exec(db, objects);
    sqlite3_free(objects);
    if (result == 0) {
        char *restore = sqlite3_mprintf("UPDATE sqlite_sequence SET seq=max(seq,%lld) WHERE name='trans_cache'", sequence);
        result = restore ? schema_exec(db, restore) : -1;
        sqlite3_free(restore);
    }
    return result;
}

/* source_hint objects (PLAN_SOURCE_HINT_METADATA.md section 6.1). They live
 * beside trans_cache so user_version stays 2 and a binary without hint support
 * keeps opening the database. The trigger and the view are owned by this
 * program and replaced on every start; CREATE ... IF NOT EXISTS would keep an
 * older definition. Runs after any v0/v1 migration: that migration's DROP
 * TABLE removes triggers on trans_cache without firing them. */
static const char *SQL_HINT_SCHEMA =
    "CREATE TABLE IF NOT EXISTS trans_cache_source_hint ("
    "  hash TEXT PRIMARY KEY NOT NULL CHECK(length(hash)=64),"
    "  source_hint TEXT NOT NULL CHECK(length(source_hint)=3 AND source_hint GLOB '[a-z][a-z][a-z]'),"
    "  mixed INTEGER NOT NULL CHECK(typeof(mixed)='integer' AND mixed IN (0,1)),"
    "  origin TEXT NOT NULL CHECK(origin IN ('model','script')),"
    "  hint_version INTEGER NOT NULL CHECK(typeof(hint_version)='integer' AND hint_version >= 1),"
    "  updated_at INTEGER NOT NULL CHECK(typeof(updated_at)='integer'));"
    "DROP TRIGGER IF EXISTS trans_cache_source_hint_delete;"
    "CREATE TRIGGER trans_cache_source_hint_delete AFTER DELETE ON trans_cache"
    "  BEGIN DELETE FROM trans_cache_source_hint WHERE hash = OLD.hash; END;"
    "CREATE TABLE IF NOT EXISTS trans_cache_hint_code (code TEXT PRIMARY KEY NOT NULL) WITHOUT ROWID;"
    "DELETE FROM trans_cache_hint_code;";

static const char *SQL_HINT_VIEW =
    "DROP VIEW IF EXISTS trans_cache_effective_source;"
    "CREATE VIEW trans_cache_effective_source AS"
    "  SELECT c.id, c.hash, c.from_lang, c.to_lang,"
    "         CASE WHEN c.from_lang = 'auto' THEN h.source_hint ELSE c.from_lang END AS effective_source,"
    "         h.mixed, h.origin, h.hint_version"
    "  FROM trans_cache c"
    "  LEFT JOIN trans_cache_source_hint h"
    "    ON h.hash = c.hash AND h.source_hint IN (SELECT code FROM trans_cache_hint_code);";

static int apply_hint_schema(sqlite3 *db) {
    if (schema_exec(db, SQL_HINT_SCHEMA) != 0) return -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO trans_cache_hint_code(code) VALUES (?)", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int rc = SQLITE_DONE;
    for (size_t i = 0; rc == SQLITE_DONE; i++) {
        const char *code = iso_639_2_code_at(i);   /* the one canonical list */
        if (!code) break;
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, code, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
    }
    if (rc == SQLITE_DONE) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, "und", -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        LOG_INFO("Cache schema error: %s", sqlite3_errmsg(db));
        return -1;
    }
    return schema_exec(db, SQL_HINT_VIEW);
}

/* Version 0 is used by the former runtime schema; version 1 by the SQL file. */
static int apply_schema(sqlite3 *db) {
    sqlite3_int64 version = 0, legacy = 0;
    char *ddl = NULL;
    sqlite3_stmt *stmt = NULL;
    if (schema_integer(db, "PRAGMA legacy_alter_table", &legacy) != 0) return -1;
    if (schema_exec(db, "PRAGMA legacy_alter_table=ON; BEGIN IMMEDIATE;") != 0) goto fail;
    if (schema_integer(db, "PRAGMA user_version", &version) != 0 || version > 2 || version < 0) goto fail;
    if (sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master WHERE name='trans_cache' AND type='table'", -1, &stmt, NULL) != SQLITE_OK) goto fail;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) ddl = strdup((const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto fail;
    if (rc == SQLITE_ROW && !ddl) goto fail;

    if (!ddl) {
        if (version == 2 || schema_exec(db, SQL_CREATE_TABLE) != 0) goto fail;
    } else {
        regmatch_t match;
        if (!known_cache_columns(db)) goto fail;
        if (source_constraint(ddl, false, &match)) {
            if (version == 2 || migrate_source_constraint(db, ddl, match) != 0) goto fail;
        } else if (!source_constraint(ddl, true, &match)) goto fail;
    }

    const char *indexes[] = {
        SQL_CREATE_INDEX_HASH,
        SQL_CREATE_INDEX_LANG_PAIR,
        SQL_CREATE_INDEX_LAST_USED,
        SQL_CREATE_INDEX_COUNT,
        SQL_CREATE_INDEX_LANG_HASH
    };

    for (size_t i = 0; i < sizeof(indexes) / sizeof(indexes[0]); i++) {
        if (schema_exec(db, indexes[i]) != 0) goto fail;
    }
    if (apply_hint_schema(db) != 0) goto fail;
    if (schema_exec(db, "PRAGMA user_version=2; COMMIT;") != 0) goto fail;
    free(ddl);
    if (!legacy) schema_exec(db, "PRAGMA legacy_alter_table=OFF;");
    return 0;

fail:
    LOG_INFO("Error: Cannot initialize/migrate translation cache schema (version %lld): %s",
             (long long)version, sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
    free(ddl);
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    if (!legacy) sqlite3_exec(db, "PRAGMA legacy_alter_table=OFF", NULL, NULL, NULL);
    return -1;
}

/* Apply PRAGMA optimizations */
/* Canonical name for an accepted journal / synchronous value, or NULL. Same lists
 * as the loader (config_loader.c); only these literals are ever put into SQL. */
static const char *canonical_name(const char *v, const char *const *names, size_t n) {
    for (size_t i = 0; v && i < n; i++) {
        if (strcasecmp(v, names[i]) == 0) return names[i];
    }
    return NULL;
}

static const char *const JOURNAL_MODES[] = { "DELETE", "TRUNCATE", "PERSIST", "MEMORY", "WAL", "OFF" };
static const char *const SYNC_MODES[] = { "OFF", "NORMAL", "FULL", "EXTRA" };

static int apply_pragmas(sqlite3 *db, const char *journal_mode, const char *sync_mode, bool keep_journal) {
    char *err_msg = NULL;
    char pragma_sql[256];
    int rc;

    const char *jm = canonical_name(journal_mode ? journal_mode : "WAL", JOURNAL_MODES, 6);
    const char *sm = canonical_name(sync_mode ? sync_mode : "NORMAL", SYNC_MODES, 4);
    if (!jm) {   /* the loader already refused it; defensive */
        LOG_INFO("Warning: unsupported SQLite journal mode, using WAL");
        jm = "WAL";
    }
    if (!sm) {
        LOG_INFO("Warning: unsupported SQLite synchronous mode, using NORMAL");
        sm = "NORMAL";
    }

    /* Set journal mode. The pragma returns the mode now in effect: SQLite keeps
     * the old one (or e.g. "memory" for an in-memory database) when it cannot
     * switch, which is a warning, not a failure; an SQL error still fails the
     * cache initialisation as before. */
    char actual_jm[32] = "";
    if (keep_journal) {   /* read only: the file keeps the mode it has */
        snprintf(pragma_sql, sizeof(pragma_sql), "PRAGMA journal_mode;");
    } else {
        snprintf(pragma_sql, sizeof(pragma_sql), "PRAGMA journal_mode=%s;", jm);
    }
    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(db, pragma_sql, -1, &st, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            const unsigned char *m = sqlite3_column_text(st, 0);
            snprintf(actual_jm, sizeof(actual_jm), "%s", m ? (const char *)m : "");
            rc = SQLITE_OK;
        } else if (rc == SQLITE_DONE) {
            rc = SQLITE_OK;
        }
    }
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error setting journal_mode: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    if (!keep_journal && strcasecmp(actual_jm, jm) != 0) {
        LOG_INFO("Warning: SQLite journal_mode is %s, not the configured %s", actual_jm[0] ? actual_jm : "unknown", jm);
    }

    /* Set synchronous mode (OFF, NORMAL, FULL, EXTRA) */
    snprintf(pragma_sql, sizeof(pragma_sql), "PRAGMA synchronous=%s;", sm);
    rc = sqlite3_exec(db, pragma_sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error setting synchronous: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    /* What is in effect on this connection (synchronous is per connection). */
    static const char *const SYNC_BY_LEVEL[] = { "OFF", "NORMAL", "FULL", "EXTRA" };
    int level = -1;
    if (sqlite3_prepare_v2(db, "PRAGMA synchronous;", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        level = sqlite3_column_int(st, 0);
    }
    if (level < 0 || level > 3) {   /* an SQL error, as for the pragmas above: fail the init */
        LOG_DEBUG("Error reading synchronous: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    LOG_INFO("SQLite journal_mode=%s, synchronous=%s", actual_jm[0] ? actual_jm : "unknown", SYNC_BY_LEVEL[level]);
    if (strcasecmp(actual_jm, "memory") == 0 || strcasecmp(actual_jm, "off") == 0 || level == 0) {
        LOG_INFO("Warning: SQLite journal_mode=%s / synchronous=%s can corrupt the cache on a crash",
                 actual_jm, SYNC_BY_LEVEL[level]);
    }

    /* Set cache size (2000 pages = ~2MB with 1KB page size) */
    rc = sqlite3_exec(db, "PRAGMA cache_size=2000;", NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error setting cache_size: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    /* Set memory-mapped I/O (256MB) */
    rc = sqlite3_exec(db, "PRAGMA mmap_size=268435456;", NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error setting mmap_size: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    return 0;
}

/* Prepare all SQL statements */
static int prepare_statements(SqliteBackendContext *ctx) {
    int rc;

    /* Lookup by hash */
    const char *sql_lookup =
        "SELECT c.id, c.hash, c.from_lang, c.to_lang, c.source_text, c.translated_text, "
        "c.count, c.last_used, c.created_at, h.source_hint, h.mixed, h.origin, h.hint_version "
        "FROM trans_cache c LEFT JOIN trans_cache_source_hint h ON h.hash = c.hash WHERE c.hash = ?;";
    rc = sqlite3_prepare_v2(ctx->db, sql_lookup, -1, &ctx->stmt_lookup, NULL);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error preparing lookup statement: %s\n", sqlite3_errmsg(ctx->db));
        return -1;
    }

    /* Insert new entry */
    const char *sql_insert =
        "INSERT INTO trans_cache (hash, from_lang, to_lang, source_text, "
        "translated_text, count, last_used, created_at) "
        "VALUES (?, ?, ?, ?, ?, 1, ?, ?);";
    rc = sqlite3_prepare_v2(ctx->db, sql_insert, -1, &ctx->stmt_insert, NULL);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error preparing insert statement: %s\n", sqlite3_errmsg(ctx->db));
        return -1;
    }

    /* Update count and last_used */
    const char *sql_update_count =
        "UPDATE trans_cache SET count = ?, last_used = ? WHERE hash = ?;";
    rc = sqlite3_prepare_v2(ctx->db, sql_update_count, -1, &ctx->stmt_update_count, NULL);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error preparing update_count statement: %s\n", sqlite3_errmsg(ctx->db));
        return -1;
    }

    /* Update translation */
    const char *sql_update_trans =
        "UPDATE trans_cache SET translated_text = ?, count = 1, last_used = ? WHERE hash = ?;";
    rc = sqlite3_prepare_v2(ctx->db, sql_update_trans, -1, &ctx->stmt_update_trans, NULL);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error preparing update_translation statement: %s\n", sqlite3_errmsg(ctx->db));
        return -1;
    }

    /* Upsert a hint for an existing auto row, never lowering its priority
     * (model > script, then hint_version); a stored code outside the code
     * table counts as no hint. ?1 is reused for the parent check. */
    const char *sql_hint_upsert =
        "INSERT INTO trans_cache_source_hint(hash, source_hint, mixed, origin, hint_version, updated_at) "
        "SELECT ?1, ?2, ?3, ?4, ?5, ?6 WHERE EXISTS "
        "(SELECT 1 FROM trans_cache WHERE hash = ?1 AND from_lang = 'auto') "
        "ON CONFLICT(hash) DO UPDATE SET source_hint = excluded.source_hint, mixed = excluded.mixed, "
        "origin = excluded.origin, hint_version = excluded.hint_version, updated_at = excluded.updated_at "
        "WHERE (CASE excluded.origin WHEN 'model' THEN 2 ELSE 1 END) > "
        "      (CASE trans_cache_source_hint.origin WHEN 'model' THEN 2 ELSE 1 END) "
        "   OR ((CASE excluded.origin WHEN 'model' THEN 2 ELSE 1 END) = "
        "       (CASE trans_cache_source_hint.origin WHEN 'model' THEN 2 ELSE 1 END) "
        "       AND excluded.hint_version >= trans_cache_source_hint.hint_version) "
        "   OR trans_cache_source_hint.source_hint NOT IN (SELECT code FROM trans_cache_hint_code);";
    rc = sqlite3_prepare_v2(ctx->db, sql_hint_upsert, -1, &ctx->stmt_hint_upsert, NULL);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error preparing hint upsert statement: %s\n", sqlite3_errmsg(ctx->db));
        return -1;
    }

    /* Delete old entries */
    const char *sql_delete_old =
        "DELETE FROM trans_cache WHERE last_used < ?;";
    rc = sqlite3_prepare_v2(ctx->db, sql_delete_old, -1, &ctx->stmt_delete_old, NULL);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error preparing delete_old statement: %s\n", sqlite3_errmsg(ctx->db));
        return -1;
    }

    /* Count all entries */
    const char *sql_count_all =
        "SELECT COUNT(*) FROM trans_cache;";
    rc = sqlite3_prepare_v2(ctx->db, sql_count_all, -1, &ctx->stmt_count_all, NULL);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error preparing count_all statement: %s\n", sqlite3_errmsg(ctx->db));
        return -1;
    }

    return 0;
}

/* Initialize SQLite backend */
TransCache *sqlite_backend_init(const char *db_path, const SqliteCacheOptions *opts) {
    if (!db_path) {
        LOG_DEBUG("Error: NULL database path\n");
        return NULL;
    }

    /* Allocate TransCache */
    TransCache *cache = calloc(1, sizeof(TransCache));
    if (!cache) {
        LOG_DEBUG("Error: Memory allocation failed\n");
        return NULL;
    }

    /* Allocate SqliteBackendContext */
    SqliteBackendContext *ctx = calloc(1, sizeof(SqliteBackendContext));
    if (!ctx) {
        LOG_DEBUG("Error: Memory allocation failed\n");
        free(cache);
        return NULL;
    }

    ctx->db_path = strdup(db_path);
    if (!ctx->db_path) {
        LOG_DEBUG("Error: Memory allocation failed\n");
        free(ctx);
        free(cache);
        return NULL;
    }

    /* Open database with FULLMUTEX for thread safety */
    int rc = sqlite3_open_v2(db_path, &ctx->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error opening database %s: %s\n", db_path, sqlite3_errmsg(ctx->db));
        free(ctx->db_path);
        free(ctx);
        free(cache);
        return NULL;
    }

    /* Apply schema */
    if (apply_schema(ctx->db) != 0) {
        LOG_DEBUG("Error: Failed to apply schema\n");
        sqlite3_close(ctx->db);
        free(ctx->db_path);
        free(ctx);
        free(cache);
        return NULL;
    }

    /* Apply PRAGMA settings after the schema work, as before (configured, else WAL / NORMAL) */
    if (apply_pragmas(ctx->db, opts ? opts->journal_mode : NULL, opts ? opts->synchronous : NULL,
                      opts && opts->keep_journal_mode) != 0) {
        LOG_DEBUG("Error: Failed to apply PRAGMA settings\n");
        sqlite3_close(ctx->db);
        free(ctx->db_path);
        free(ctx);
        free(cache);
        return NULL;
    }

    /* Prepare statements */
    if (prepare_statements(ctx) != 0) {
        LOG_DEBUG("Error: Failed to prepare statements\n");
        sqlite3_close(ctx->db);
        free(ctx->db_path);
        free(ctx);
        free(cache);
        return NULL;
    }

    /* Initialize read-write lock */
    if (pthread_rwlock_init(&cache->lock, NULL) != 0) {
        LOG_DEBUG("Error: Failed to initialize rwlock\n");
        sqlite3_finalize(ctx->stmt_lookup);
        sqlite3_finalize(ctx->stmt_insert);
        sqlite3_finalize(ctx->stmt_update_count);
        sqlite3_finalize(ctx->stmt_update_trans);
        sqlite3_finalize(ctx->stmt_delete_old);
        sqlite3_finalize(ctx->stmt_count_all);
        sqlite3_finalize(ctx->stmt_hint_upsert);
        sqlite3_close(ctx->db);
        free(ctx->db_path);
        free(ctx);
        free(cache);
        return NULL;
    }

    /* Set up TransCache */
    cache->type = CACHE_BACKEND_SQLITE;
    cache->backend_ctx = ctx;
    cache->ops = sqlite_backend_get_ops();

    LOG_INFO("SQLite cache initialized: %s\n", db_path);

    return cache;
}

/* Hint columns (source_hint, mixed, origin, hint_version) starting at `col`;
 * NULL or anything invalid means "no hint". Also used by cache_tool. */
void sqlite_backend_read_hint(sqlite3_stmt *stmt, int col, CacheEntry *entry) {
    memset(&entry->hint, 0, sizeof(entry->hint));
    if (strcmp(entry->from_lang, "auto") != 0 || sqlite3_column_type(stmt, col) != SQLITE_TEXT ||
        sqlite3_column_type(stmt, col + 1) != SQLITE_INTEGER || sqlite3_column_type(stmt, col + 2) != SQLITE_TEXT ||
        sqlite3_column_type(stmt, col + 3) != SQLITE_INTEGER) return;
    /* Read as 64-bit and range-check before narrowing: sqlite3_column_int()
     * would turn 4294967297 into a "valid" 1. */
    sqlite3_int64 mixed = sqlite3_column_int64(stmt, col + 1);
    sqlite3_int64 version = sqlite3_column_int64(stmt, col + 3);
    if ((mixed != 0 && mixed != 1) || version < 1 || version > INT_MAX) return;
    const char *lang = (const char *)sqlite3_column_text(stmt, col);
    const char *origin = (const char *)sqlite3_column_text(stmt, col + 2);
    if (!lang || !origin || strlen(lang) != 3) return;
    SourceHint h = { .mixed = mixed == 1, .version = (int)version };
    memcpy(h.lang, lang, 4);
    if (strcmp(origin, "model") == 0) h.origin = SOURCE_HINT_ORIGIN_MODEL;
    else if (strcmp(origin, "script") == 0) h.origin = SOURCE_HINT_ORIGIN_SCRIPT;
    if (source_hint_valid(&h)) entry->hint = h;
}

/* Lookup cache entry */
static CacheEntry* sqlite_backend_lookup(void *backend_ctx,
                                          const char *from_lang,
                                          const char *to_lang,
                                          const char *text) {
    if (!backend_ctx || !from_lang || !to_lang || !text) {
        return NULL;
    }

    SqliteBackendContext *ctx = (SqliteBackendContext*)backend_ctx;

    /* Calculate hash */
    char hash[65];
    trans_cache_calculate_hash(from_lang, to_lang, text, hash);

    /* Bind hash parameter */
    sqlite3_reset(ctx->stmt_lookup);
    sqlite3_bind_text(ctx->stmt_lookup, 1, hash, -1, SQLITE_STATIC);

    /* Execute query */
    int rc = sqlite3_step(ctx->stmt_lookup);
    if (rc != SQLITE_ROW) {
        /* Not found or error */
        sqlite3_reset(ctx->stmt_lookup);
        return NULL;
    }

    /* Allocate cache entry */
    CacheEntry *entry = calloc(1, sizeof(CacheEntry));
    if (!entry) {
        sqlite3_reset(ctx->stmt_lookup);
        return NULL;
    }

    /* Extract data from result row */
    entry->id = sqlite3_column_int(ctx->stmt_lookup, 0);
    strncpy(entry->hash, (const char*)sqlite3_column_text(ctx->stmt_lookup, 1), sizeof(entry->hash) - 1);
    strncpy(entry->from_lang, (const char*)sqlite3_column_text(ctx->stmt_lookup, 2), sizeof(entry->from_lang) - 1);
    strncpy(entry->to_lang, (const char*)sqlite3_column_text(ctx->stmt_lookup, 3), sizeof(entry->to_lang) - 1);
    entry->source_text = strdup((const char*)sqlite3_column_text(ctx->stmt_lookup, 4));
    entry->translated_text = strdup((const char*)sqlite3_column_text(ctx->stmt_lookup, 5));
    entry->count = sqlite3_column_int(ctx->stmt_lookup, 6);
    entry->last_used = (time_t)sqlite3_column_int64(ctx->stmt_lookup, 7);
    entry->created_at = (time_t)sqlite3_column_int64(ctx->stmt_lookup, 8);
    sqlite_backend_read_hint(ctx->stmt_lookup, 9, entry);

    sqlite3_reset(ctx->stmt_lookup);

    if (!entry->source_text || !entry->translated_text) {
        free(entry->source_text);
        free(entry->translated_text);
        free(entry);
        return NULL;
    }

    return entry;
}

/* Add new cache entry */
static int sqlite_backend_add(void *backend_ctx,
                              const char *from_lang,
                              const char *to_lang,
                              const char *source_text,
                              const char *translated_text) {
    if (!backend_ctx || !from_lang || !to_lang || !source_text || !translated_text) {
        return -1;
    }

    SqliteBackendContext *ctx = (SqliteBackendContext*)backend_ctx;

    /* Calculate hash */
    char hash[65];
    trans_cache_calculate_hash(from_lang, to_lang, source_text, hash);

    /* Current timestamp */
    time_t now = time(NULL);

    /* Bind parameters */
    sqlite3_reset(ctx->stmt_insert);
    sqlite3_bind_text(ctx->stmt_insert, 1, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->stmt_insert, 2, from_lang, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->stmt_insert, 3, to_lang, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->stmt_insert, 4, source_text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->stmt_insert, 5, translated_text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ctx->stmt_insert, 6, (sqlite3_int64)now);
    sqlite3_bind_int64(ctx->stmt_insert, 7, (sqlite3_int64)now);

    /* Execute insert */
    int rc = sqlite3_step(ctx->stmt_insert);
    sqlite3_reset(ctx->stmt_insert);

    if (rc != SQLITE_DONE) {
        LOG_DEBUG("Error inserting cache entry: %s\n", sqlite3_errmsg(ctx->db));
        return -1;
    }

    return 0;
}

/* Update cache entry count */
static int sqlite_backend_update_count(void *backend_ctx, CacheEntry *entry) {
    if (!backend_ctx || !entry) {
        return -1;
    }

    SqliteBackendContext *ctx = (SqliteBackendContext*)backend_ctx;

    /* Increment count and update last_used */
    entry->count++;
    entry->last_used = time(NULL);

    /* Bind parameters */
    sqlite3_reset(ctx->stmt_update_count);
    sqlite3_bind_int(ctx->stmt_update_count, 1, entry->count);
    sqlite3_bind_int64(ctx->stmt_update_count, 2, (sqlite3_int64)entry->last_used);
    sqlite3_bind_text(ctx->stmt_update_count, 3, entry->hash, -1, SQLITE_STATIC);

    /* Execute update */
    int rc = sqlite3_step(ctx->stmt_update_count);
    sqlite3_reset(ctx->stmt_update_count);

    if (rc != SQLITE_DONE) {
        LOG_DEBUG("Error updating count: %s\n", sqlite3_errmsg(ctx->db));
        return -1;
    }

    return 0;
}

/* Update cache entry translation */
static int sqlite_backend_update_translation(void *backend_ctx,
                                              CacheEntry *entry,
                                              const char *new_translation) {
    if (!backend_ctx || !entry || !new_translation) {
        return -1;
    }

    SqliteBackendContext *ctx = (SqliteBackendContext*)backend_ctx;

    /* Update in-memory entry */
    free(entry->translated_text);
    entry->translated_text = strdup(new_translation);
    if (!entry->translated_text) {
        return -1;
    }

    /* Reset count to 1 and update last_used */
    entry->count = 1;
    entry->last_used = time(NULL);

    /* Bind parameters */
    sqlite3_reset(ctx->stmt_update_trans);
    sqlite3_bind_text(ctx->stmt_update_trans, 1, new_translation, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ctx->stmt_update_trans, 2, (sqlite3_int64)entry->last_used);
    sqlite3_bind_text(ctx->stmt_update_trans, 3, entry->hash, -1, SQLITE_STATIC);

    /* Execute update */
    int rc = sqlite3_step(ctx->stmt_update_trans);
    sqlite3_reset(ctx->stmt_update_trans);

    if (rc != SQLITE_DONE) {
        LOG_DEBUG("Error updating translation: %s\n", sqlite3_errmsg(ctx->db));
        return -1;
    }

    return 0;
}

/* Result of one parent mutation attempt, kept apart from the caller's copy
 * until COMMIT succeeds (PLAN_SOURCE_HINT_METADATA.md section 6.3). */
typedef struct {
    int count;
    time_t last_used;
    bool translation_changed;
    bool reinserted;          /* the row vanished since the lookup: its old hint went with it */
} StagedParent;

static int step_done(sqlite3_stmt *stmt) {
    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int insert_parent(SqliteBackendContext *ctx, const char *hash, const char *from_lang,
                         const char *to_lang, const char *source_text, const char *translated_text,
                         time_t now) {
    sqlite3_stmt *st = ctx->stmt_insert;
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, from_lang, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, to_lang, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, source_text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, translated_text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)now);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)now);
    return step_done(st);
}

/* The same mutation as add / update_count / update_translation, but nothing is
 * written into `cached`. A row that vanished since the lookup (0 rows changed,
 * read right after the parent statement) is inserted again. */
static int mutate_parent(SqliteBackendContext *ctx, const char *hash, const char *from_lang,
                         const char *to_lang, const char *source_text, const char *translated_text,
                         const CacheEntry *cached, StagedParent *staged) {
    time_t now = time(NULL);
    staged->last_used = now;
    staged->reinserted = false;
    staged->translation_changed = cached && strcmp(cached->translated_text, translated_text) != 0;
    if (!cached) {
        staged->count = 1;
        return insert_parent(ctx, hash, from_lang, to_lang, source_text, translated_text, now);
    }

    sqlite3_stmt *st;
    if (!staged->translation_changed) {
        staged->count = cached->count + 1;            /* update_count semantics */
        st = ctx->stmt_update_count;
        sqlite3_reset(st);
        sqlite3_bind_int(st, 1, staged->count);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
        sqlite3_bind_text(st, 3, hash, -1, SQLITE_STATIC);
    } else {
        staged->count = 1;                            /* update_translation semantics */
        st = ctx->stmt_update_trans;
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, translated_text, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
        sqlite3_bind_text(st, 3, hash, -1, SQLITE_STATIC);
    }
    if (step_done(st) != 0) return -1;
    if (sqlite3_changes(ctx->db) == 0) {
        staged->count = 1;
        staged->reinserted = true;
        staged->translation_changed = strcmp(cached->translated_text, translated_text) != 0;
        return insert_parent(ctx, hash, from_lang, to_lang, source_text, translated_text, now);
    }
    return 0;
}

/* Returns 1 if the stored hint was replaced, 0 if priority kept the old one,
 * -1 on error. */
static int upsert_hint(SqliteBackendContext *ctx, const char *hash, const SourceHint *hint) {
    sqlite3_stmt *st = ctx->stmt_hint_upsert;
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, hint->lang, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, hint->mixed ? 1 : 0);
    sqlite3_bind_text(st, 4, hint->origin == SOURCE_HINT_ORIGIN_MODEL ? "model" : "script", -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 5, hint->version);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)time(NULL));
    if (step_done(st) != 0) return -1;
    return sqlite3_changes(ctx->db) > 0 ? 1 : 0;
}

/* End any open transaction. A failed ROLLBACK (e.g. out of memory) would leave
 * the shared connection inside a transaction holding the write lock, so it is
 * retried; if it still cannot finish, that is logged as an error. */
static void rollback_all(SqliteBackendContext *ctx) {
    for (int attempt = 0; attempt < 3 && !sqlite3_get_autocommit(ctx->db); attempt++)
        sqlite3_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    if (!sqlite3_get_autocommit(ctx->db))
        LOG_INFO("Error: cache connection is stuck in a transaction (%s); cache writes will fail",
                 sqlite3_errmsg(ctx->db));
}

/* BEGIN IMMEDIATE -> parent -> SAVEPOINT hint -> hint -> RELEASE -> COMMIT.
 * - A hint statement error that leaves the transaction open is undone with
 *   ROLLBACK TO; the parent still commits.
 * - Some errors (e.g. SQLITE_FULL) abort the whole transaction, parent
 *   included: then the parent alone is retried once, from fresh staging.
 * - SQLITE_BUSY from another process is not waited for (best-effort cache).
 * - The deadline is checked again right before COMMIT. */
static int sqlite_backend_store_result(void *backend_ctx, const char *from_lang, const char *to_lang,
                                       const char *source_text, const char *translated_text,
                                       CacheEntry *cached, const SourceHint *hint,
                                       const TranslationControl *control) {
    if (!backend_ctx) return -1;
    SqliteBackendContext *ctx = (SqliteBackendContext *)backend_ctx;
    char hash[65];
    trans_cache_calculate_hash(from_lang, to_lang, source_text, hash);

    /* A ROLLBACK that failed before (rollback_all() logged "stuck") leaves this shared
     * connection inside that transaction, holding the write lock, and every later
     * BEGIN fails until a restart: end it first (PLAN_FAULT_INJECTION.md Q1). This
     * store is then skipped: `cached` may come from a lookup that saw the writes just
     * rolled back (measured: it would commit A x2 while the copy says B x2). From the
     * next lookup on, everything is read from the committed state again. */
    if (!sqlite3_get_autocommit(ctx->db)) {
        rollback_all(ctx);
        LOG_INFO("Cache write skipped: ended a transaction an earlier failure left open%s",
                 sqlite3_get_autocommit(ctx->db) ? "" : " (still open)");
        return -1;
    }

    char *caller_copy = NULL;          /* allocated before anything is written */
    if (cached && strcmp(cached->translated_text, translated_text) != 0) {
        caller_copy = strdup(translated_text);
        if (!caller_copy) return -1;
    }

    const SourceHint *want_hint = hint;
    for (int attempt = 0; attempt < 2; attempt++) {
        StagedParent staged;
        int hint_written = 0;
        if (sqlite3_exec(ctx->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
            LOG_INFO("Cache write skipped: %s", sqlite3_errmsg(ctx->db));   /* BUSY: fail fast */
            break;
        }
        if (mutate_parent(ctx, hash, from_lang, to_lang, source_text, translated_text, cached, &staged) != 0) {
            LOG_DEBUG("Cache parent write failed: %s\n", sqlite3_errmsg(ctx->db));
            rollback_all(ctx);
            break;
        }
        if (want_hint) {
            int rc = sqlite3_exec(ctx->db, "SAVEPOINT hint", NULL, NULL, NULL) == SQLITE_OK
                         ? upsert_hint(ctx, hash, want_hint) : -1;
            if (rc < 0) {
                if (sqlite3_get_autocommit(ctx->db)) {
                    /* The error ended the whole transaction: retry the parent only. */
                    LOG_INFO("Cache hint write aborted the transaction (%s); retrying without the hint",
                             sqlite3_errmsg(ctx->db));
                    want_hint = NULL;
                    continue;
                }
                LOG_INFO("Cache hint write failed (%s); keeping the translation", sqlite3_errmsg(ctx->db));
                if (sqlite3_exec(ctx->db, "ROLLBACK TO hint", NULL, NULL, NULL) != SQLITE_OK ||
                    sqlite3_exec(ctx->db, "RELEASE hint", NULL, NULL, NULL) != SQLITE_OK) {
                    /* The failed hint could not be undone alone: never COMMIT it,
                     * end everything and retry the parent only. */
                    rollback_all(ctx);
                    want_hint = NULL;
                    continue;
                }
            } else {
                hint_written = rc;
                sqlite3_exec(ctx->db, "RELEASE hint", NULL, NULL, NULL);
            }
        }
        if (!translation_remaining_ms(control) ||
            sqlite3_exec(ctx->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            rollback_all(ctx);
            break;
        }
        if (cached) {   /* committed: mirror the staged result into the caller's copy */
            if (caller_copy) {
                free(cached->translated_text);
                cached->translated_text = caller_copy;
                caller_copy = NULL;
            }
            cached->count = staged.count;
            cached->last_used = staged.last_used;
            if (hint_written) cached->hint = *want_hint;
            else if (staged.reinserted) memset(&cached->hint, 0, sizeof(cached->hint));   /* the old hint was deleted with the row */
        }
        free(caller_copy);
        return 0;
    }
    free(caller_copy);
    return -1;
}

static int sqlite_backend_merge_hint(void *backend_ctx, const char *hash, const SourceHint *hint) {
    if (!backend_ctx || !hash) return -1;
    return upsert_hint((SqliteBackendContext *)backend_ctx, hash, hint);   /* one autocommit statement */
}

/* Save cache (no-op for SQLite - auto-commit) */
static int sqlite_backend_save(void *backend_ctx) {
    (void)backend_ctx;
    /* SQLite auto-commits, no explicit save needed */
    return 0;
}

/* Cleanup old cache entries */
static int sqlite_backend_cleanup(void *backend_ctx, int days_threshold) {
    if (!backend_ctx || days_threshold <= 0) {
        return 0;
    }

    SqliteBackendContext *ctx = (SqliteBackendContext*)backend_ctx;

    /* Calculate threshold timestamp */
    int64_t threshold_time = trans_cache_cutoff((int64_t)time(NULL), days_threshold);

    /* Bind threshold parameter */
    sqlite3_reset(ctx->stmt_delete_old);
    sqlite3_bind_int64(ctx->stmt_delete_old, 1, (sqlite3_int64)threshold_time);

    /* Execute delete */
    int rc = sqlite3_step(ctx->stmt_delete_old);
    int removed_count = sqlite3_changes(ctx->db);
    sqlite3_reset(ctx->stmt_delete_old);

    if (rc != SQLITE_DONE) {
        LOG_DEBUG("Error cleaning up old entries: %s\n", sqlite3_errmsg(ctx->db));
        return 0;
    }

    return removed_count;
}

/* Get cache statistics */
static void sqlite_backend_stats(void *backend_ctx,
                                  size_t *total_entries,
                                  size_t *active_entries,
                                  size_t *expired_entries,
                                  int cache_threshold,
                                  int days_threshold) {
    if (!backend_ctx) {
        return;
    }

    SqliteBackendContext *ctx = (SqliteBackendContext*)backend_ctx;

    /* Get total entries */
    sqlite3_reset(ctx->stmt_count_all);
    if (sqlite3_step(ctx->stmt_count_all) == SQLITE_ROW) {
        if (total_entries) {
            *total_entries = sqlite3_column_int(ctx->stmt_count_all, 0);
        }
    }
    sqlite3_reset(ctx->stmt_count_all);

    /* Get active entries (count >= threshold) */
    if (active_entries) {
        sqlite3_stmt *stmt = NULL;
        const char *sql = "SELECT COUNT(*) FROM trans_cache WHERE count >= ?;";
        if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, cache_threshold);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                *active_entries = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    /* Get expired entries (last_used < threshold) */
    if (expired_entries) {
        int64_t threshold_time = trans_cache_cutoff((int64_t)time(NULL), days_threshold);

        sqlite3_stmt *stmt = NULL;
        const char *sql = "SELECT COUNT(*) FROM trans_cache WHERE last_used < ?;";
        if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, (sqlite3_int64)threshold_time);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                *expired_entries = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }
}

/* Free SQLite backend */
static void sqlite_backend_free(void *backend_ctx) {
    if (!backend_ctx) {
        return;
    }

    SqliteBackendContext *ctx = (SqliteBackendContext*)backend_ctx;

    /* Finalize all prepared statements */
    if (ctx->stmt_lookup) sqlite3_finalize(ctx->stmt_lookup);
    if (ctx->stmt_insert) sqlite3_finalize(ctx->stmt_insert);
    if (ctx->stmt_update_count) sqlite3_finalize(ctx->stmt_update_count);
    if (ctx->stmt_update_trans) sqlite3_finalize(ctx->stmt_update_trans);
    if (ctx->stmt_delete_old) sqlite3_finalize(ctx->stmt_delete_old);
    if (ctx->stmt_count_all) sqlite3_finalize(ctx->stmt_count_all);
    if (ctx->stmt_hint_upsert) sqlite3_finalize(ctx->stmt_hint_upsert);

    /* Close database */
    if (ctx->db) {
        sqlite3_close(ctx->db);
    }

    /* Free context */
    free(ctx->db_path);
    free(ctx);
}

/* Get backend operations */
CacheBackendOps *sqlite_backend_get_ops(void) {
    static CacheBackendOps ops = {
        .lookup = sqlite_backend_lookup,
        .add = sqlite_backend_add,
        .update_count = sqlite_backend_update_count,
        .update_translation = sqlite_backend_update_translation,
        .save = sqlite_backend_save,
        .cleanup = sqlite_backend_cleanup,
        .stats = sqlite_backend_stats,
        .free_backend = sqlite_backend_free,
        .store_result = sqlite_backend_store_result,
        .merge_hint = sqlite_backend_merge_hint
    };
    return &ops;
}
