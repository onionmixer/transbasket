/**
 * SQLite backend implementation for translation cache.
 * Uses prepared statements for high performance and SQL injection protection.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
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
    "  CHECK(length(from_lang) = 3),"
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

/* Apply database schema */
static int apply_schema(sqlite3 *db) {
    char *err_msg = NULL;
    int rc;

    /* Create table */
    rc = sqlite3_exec(db, SQL_CREATE_TABLE, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error creating table: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    /* Create indexes */
    const char *indexes[] = {
        SQL_CREATE_INDEX_HASH,
        SQL_CREATE_INDEX_LANG_PAIR,
        SQL_CREATE_INDEX_LAST_USED,
        SQL_CREATE_INDEX_COUNT,
        SQL_CREATE_INDEX_LANG_HASH
    };

    for (size_t i = 0; i < sizeof(indexes) / sizeof(indexes[0]); i++) {
        rc = sqlite3_exec(db, indexes[i], NULL, NULL, &err_msg);
        if (rc != SQLITE_OK) {
            LOG_DEBUG("Error creating index: %s\n", err_msg);
            sqlite3_free(err_msg);
            return -1;
        }
    }

    return 0;
}

/* Apply PRAGMA optimizations */
static int apply_pragmas(sqlite3 *db, const char *journal_mode, const char *sync_mode) {
    char *err_msg = NULL;
    char pragma_sql[256];
    int rc;

    /* Set journal mode (WAL, DELETE, etc.) */
    snprintf(pragma_sql, sizeof(pragma_sql), "PRAGMA journal_mode=%s;", journal_mode);
    rc = sqlite3_exec(db, pragma_sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error setting journal_mode: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    /* Set synchronous mode (FULL, NORMAL, OFF) */
    snprintf(pragma_sql, sizeof(pragma_sql), "PRAGMA synchronous=%s;", sync_mode);
    rc = sqlite3_exec(db, pragma_sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        LOG_DEBUG("Error setting synchronous: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
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
        "SELECT id, hash, from_lang, to_lang, source_text, translated_text, "
        "count, last_used, created_at FROM trans_cache WHERE hash = ?;";
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
TransCache *sqlite_backend_init(const char *db_path) {
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

    /* Apply PRAGMA optimizations (defaults: WAL, NORMAL) */
    if (apply_pragmas(ctx->db, "WAL", "NORMAL") != 0) {
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
    time_t now = time(NULL);
    time_t threshold_time = now - (days_threshold * 24 * 60 * 60);

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
        time_t now = time(NULL);
        time_t threshold_time = now - (days_threshold * 24 * 60 * 60);

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
        .free_backend = sqlite_backend_free
    };
    return &ops;
}
