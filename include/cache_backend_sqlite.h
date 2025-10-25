/**
 * SQLite backend for translation cache.
 * Provides high-performance database storage with prepared statements.
 */

#ifndef CACHE_BACKEND_SQLITE_H
#define CACHE_BACKEND_SQLITE_H

#include <sqlite3.h>
#include "trans_cache.h"

/**
 * SQLite backend context structure.
 * Stores database handle and prepared statements for efficient operations.
 */
typedef struct {
    sqlite3 *db;                    /* SQLite database handle */
    char *db_path;                  /* Database file path */

    /* Prepared statements for common operations */
    sqlite3_stmt *stmt_lookup;      /* SELECT by hash */
    sqlite3_stmt *stmt_insert;      /* INSERT new entry */
    sqlite3_stmt *stmt_update_count;/* UPDATE count and last_used */
    sqlite3_stmt *stmt_update_trans;/* UPDATE translation */
    sqlite3_stmt *stmt_delete_old;  /* DELETE old entries */
    sqlite3_stmt *stmt_count_all;   /* COUNT(*) */
} SqliteBackendContext;

/**
 * Initialize SQLite backend.
 * Creates database and tables if they don't exist.
 *
 * @param db_path Path to SQLite database file
 * @return TransCache instance or NULL on error
 */
TransCache *sqlite_backend_init(const char *db_path);

/**
 * Get SQLite backend operations table.
 *
 * @return Pointer to static CacheBackendOps structure
 */
CacheBackendOps *sqlite_backend_get_ops(void);

#endif /* CACHE_BACKEND_SQLITE_H */
