-- Translation Cache SQLite Schema
-- This schema is designed to store translation cache entries with optimal indexing
-- for fast lookup and efficient cleanup operations.

-- Main cache table
CREATE TABLE IF NOT EXISTS trans_cache (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    hash TEXT NOT NULL UNIQUE,              -- SHA256 hash of from|to|source_text (64 chars)
    from_lang TEXT NOT NULL,                -- ISO 639-2 source language code (3 chars)
    to_lang TEXT NOT NULL,                  -- ISO 639-2 target language code (3 chars)
    source_text TEXT NOT NULL,              -- Original text to translate
    translated_text TEXT NOT NULL,          -- Translated result
    count INTEGER DEFAULT 1,                -- Number of times this translation was requested
    last_used INTEGER NOT NULL,             -- Last access timestamp (Unix epoch seconds)
    created_at INTEGER NOT NULL,            -- Creation timestamp (Unix epoch seconds)

    -- Constraints
    CHECK(length(hash) = 64),               -- Ensure valid SHA256 hex string
    CHECK(length(from_lang) = 3),           -- Ensure valid ISO 639-2 code
    CHECK(length(to_lang) = 3),             -- Ensure valid ISO 639-2 code
    CHECK(count >= 1),                      -- Count must be at least 1
    CHECK(last_used >= created_at)          -- Last used cannot be before creation
);

-- Primary index on hash for O(1) lookup
-- This is the most critical index as all cache lookups use hash
CREATE UNIQUE INDEX IF NOT EXISTS idx_hash ON trans_cache(hash);

-- Composite index for language pair queries
-- Useful for cache statistics and language-specific operations
CREATE INDEX IF NOT EXISTS idx_lang_pair ON trans_cache(from_lang, to_lang);

-- Index on last_used for cleanup operations
-- Enables efficient deletion of old entries (e.g., older than N days)
CREATE INDEX IF NOT EXISTS idx_last_used ON trans_cache(last_used);

-- Index on count for analytics and popular translation queries
-- Allows fast retrieval of most frequently used translations
CREATE INDEX IF NOT EXISTS idx_count ON trans_cache(count DESC);

-- Composite index for language pair + hash lookup optimization
-- Optimizes queries that filter by language pair first
CREATE INDEX IF NOT EXISTS idx_lang_hash ON trans_cache(from_lang, to_lang, hash);

-- Schema version metadata
-- Used for database migration tracking
PRAGMA user_version = 1;

-- Performance optimizations
-- Enable Write-Ahead Logging for better concurrency
PRAGMA journal_mode = WAL;

-- Synchronous mode: NORMAL provides good balance of safety and speed
-- FULL = maximum safety but slower
-- NORMAL = safe for most cases with better performance
-- OFF = fastest but risk of corruption on power loss
PRAGMA synchronous = NORMAL;

-- Foreign keys enforcement (not used in this schema but good practice)
PRAGMA foreign_keys = ON;

-- Cache size: allocate more memory for better performance
-- -2000 means 2000 KB (2 MB) of cache
-- Negative values are in KB, positive values are in pages
PRAGMA cache_size = -2000;

-- Temporary storage in memory for better performance
PRAGMA temp_store = MEMORY;

-- Enable memory-mapped I/O (mmap) for faster reads
-- 268435456 = 256 MB
-- Adjust based on available system memory
PRAGMA mmap_size = 268435456;

-- Analyze tables for query optimization
-- This helps SQLite choose optimal query plans
ANALYZE;

-- Optional: Full-Text Search index for source and translated text
-- Uncomment if you need to search within translation text
-- This significantly increases storage size but enables text search
/*
CREATE VIRTUAL TABLE IF NOT EXISTS trans_cache_fts USING fts5(
    source_text,
    translated_text,
    content=trans_cache,
    content_rowid=id
);

-- Triggers to keep FTS index synchronized
CREATE TRIGGER IF NOT EXISTS trans_cache_ai AFTER INSERT ON trans_cache BEGIN
    INSERT INTO trans_cache_fts(rowid, source_text, translated_text)
    VALUES (new.id, new.source_text, new.translated_text);
END;

CREATE TRIGGER IF NOT EXISTS trans_cache_ad AFTER DELETE ON trans_cache BEGIN
    INSERT INTO trans_cache_fts(trans_cache_fts, rowid, source_text, translated_text)
    VALUES('delete', old.id, old.source_text, old.translated_text);
END;

CREATE TRIGGER IF NOT EXISTS trans_cache_au AFTER UPDATE ON trans_cache BEGIN
    INSERT INTO trans_cache_fts(trans_cache_fts, rowid, source_text, translated_text)
    VALUES('delete', old.id, old.source_text, old.translated_text);
    INSERT INTO trans_cache_fts(rowid, source_text, translated_text)
    VALUES (new.id, new.source_text, new.translated_text);
END;
*/
