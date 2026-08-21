# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Transbasket is a high-performance HTTP-based translation server daemon implemented in C that receives translation requests via HTTP, processes them through OpenAI-compatible APIs, and returns translated results while maintaining the HTTP connection.

**Current Status**: Production C implementation (Python POC has been retired)

## Quick Start Commands

### Build

```bash
# Check dependencies
make check-deps

# Build
make

# Debug build
make debug

# Clean
make clean
```

### Run Server

```bash
# Basic usage
./transbasket

# With custom configuration
./transbasket -c /path/to/config.conf -p /path/to/PROMPT_PREFIX.txt

# With custom worker count
./transbasket -w 20

# Or with environment variable
MAX_WORKERS=20 ./transbasket
```

### Quick API Tests

```bash
# Health check
curl http://localhost:8889/health

# Translation request
curl -X POST http://localhost:8889/translate \
  -H "Content-Type: application/json" \
  -d '{
    "timestamp": "2025-10-10T01:23:45.678Z",
    "uuid": "550e8400-e29b-41d4-a716-446655440000",
    "from": "kor",
    "to": "eng",
    "text": "안녕하세요"
  }'
```

## Architecture

### Module Organization

The C implementation follows a clear separation of concerns across these core modules:

1. **main.c**: Entry point with signal handling (SIGINT, SIGTERM, SIGHUP)
2. **config_loader.c**: Parses `transbasket.conf` (KEY="value" format) and `PROMPT_PREFIX.txt`
3. **utils.c**: Validation utilities (RFC 3339 timestamps, UUID v4, ISO 639-2 language codes, emoji stripping)
4. **json_handler.c**: JSON request/response handling using cJSON
5. **http_client.c**: Translation dispatch — `normal` (OpenAI API via libcurl, retry/backoff) and `codex` (MCP) backends
6. **http_server.c**: HTTP server using libmicrohttpd with thread-per-connection model
7. **mcp_client.c**: Minimal MCP stdio JSON-RPC 2.0 client that drives `codex mcp-server` (used by the `codex` backend)

(Cache and language-table concerns live in `trans_cache.c`, `cache_backend_*.c`, and `language_table.c`.)

### Threading Model

**Critical architectural pattern: Thread-per-Connection**

- **libmicrohttpd**: Creates a new thread for each incoming connection
- **Blocking I/O**: Each thread can safely block on OpenAI API calls
- **Connection Persistence**: HTTP connection maintained until translation completes
- **Scalability**: Number of worker threads configured via MAX_WORKERS

This approach is efficient because:
- No async complexity needed (blocking calls are fine in dedicated threads)
- Natural backpressure (limited by thread pool size)
- Simple to reason about and debug

Implementation in `http_server.c`:
- `MHD_USE_THREAD_PER_CONNECTION` flag creates dedicated threads
- `openai_translate()` called directly (no executor needed)

For the `codex` backend the same blocking thread spawns a `codex mcp-server` child per request, so the model still maps cleanly: one blocked thread = one outstanding codex process (capped by a semaphore).

### Request Flow

1. Client → libmicrohttpd creates new thread
2. Thread → Validates JSON with cJSON (invalid → 422)
3. Thread → Cache lookup; on hit above threshold, returns cached translation
4. Thread → `openai_translate()` dispatches on `MODEL_TYPE`:
   - **normal**: builds prompt, calls OpenAI API via libcurl (retry: max 3, exponential backoff), unescapes + strips emoji/shortcodes
   - **codex**: builds a delimiter-isolated prompt, acquires a concurrency slot, spawns `codex mcp-server`, runs initialize + `tools/call`, takes `structuredContent.content` (trim + whole-fence unwrap only)
5. Thread → Updates cache
6. Thread → Sends JSON response preserving original UUID/timestamp
7. Connection closes, thread terminates

### Configuration System

Two configuration files:

1. **transbasket.conf**: Shell-style KEY="value" format (not INI)
   - Parsed with regex pattern matching
   - Custom validation for URLs, ports
   - Relative paths resolved from executable location

2. **PROMPT_PREFIX.txt**: Multi-line prompt prefix
   - Loaded at startup
   - Supports complex instructions
   - UTF-8 encoded

### Translation Backend (MODEL_TYPE)

`MODEL_TYPE` selects how translations are produced (see `config_loader.c`, `http_client.c:openai_translate()` dispatch):

- **`normal`** (default): existing OpenAI-compatible HTTP API path (`/v1/chat/completions` via libcurl). Uses `OPENAI_BASE_URL/MODEL/API_KEY`. Behavior unchanged.
- **`codex`**: drives the installed `codex` CLI as an MCP server (`codex mcp-server`, stdio JSON-RPC 2.0) via the `mcp_client.c` module. `OPENAI_*` are unused; `CODEX_*` settings apply.

codex backend specifics:
- One `codex mcp-server` child is spawned per request (`posix_spawnp`, in its own process group so the node wrapper **and** the real codex grandchild are reaped on stop).
- Concurrency is capped by a semaphore in `OpenAITranslator` (`CODEX_MAX_CONCURRENCY`, default = `MAX_WORKERS`); exhaustion → 503, call timeout → 504.
- Untrusted text is isolated with a per-request random delimiter (uuid-based); `sandbox=read-only`/`approval-policy=never` are force-validated unless `CODEX_ALLOW_UNSAFE=true`.
- Each request runs in a fresh `mkdtemp` cwd under `CODEX_RUN_DIR` (default `/var/lib/transbasket/run`), removed afterward.
- The translation is taken from `result.structuredContent.content`; codex output is **not** re-unescaped or emoji-stripped (would corrupt it), only trimmed + whole-output code-fence unwrapped.
- Requires the host (service user) to have `codex login` done and a writable `CODEX_HOME`. See `PLAN_MODEL_TYPE_CODEX_MCP.md` §7.1 for the systemd/operator checklist.
- **`TRANS_CACHE_THRESHOLD` must be `1` in codex mode.** codex output is non-deterministic, so the same source text comes back slightly reworded on each call; `handle_translate()` then treats it as a changed translation and resets the entry's `count` to 1. With any threshold above 1 the count never reaches it, the cache never serves a hit, and every request pays for a codex call. Verified: threshold 3 → 4 identical requests all took ~4 s and re-invoked codex; threshold 1 → second request served from cache in ~0.01 s.

### Translation Cache Concurrency

The cache is shared by every connection thread, so two rules are load-bearing:

- **All backend calls are serialized.** `trans_cache_lookup()/_save()/_stats()` take the *write* lock even though they read: the sqlite backend drives one shared `sqlite3_stmt` (reset/bind/step/column) and the text backend refreshes `last_used`. Running them concurrently under a read lock corrupts the statement — `sqlite3_column_text()` returns NULL and the daemon segfaults in `strncpy`. Do not "optimize" these back to `rdlock`.
- **`trans_cache_lookup()` returns a caller-owned deep copy**, in every backend. Release it with `trans_cache_entry_free()`; never hand a stored entry outside the backend, or another thread can free its strings mid-read. `update_count()`/`update_translation()` locate the stored record via `entry->hash` and keep the caller's copy in sync.

Text-backend `add()` skips insertion when the hash is already stored (two threads can miss on the same new text at once); the sqlite backend gets this from its `UNIQUE` index.

### Validation Standards

**Strictly enforced formats:**
- Timestamps: RFC 3339 (e.g., `2025-10-10T01:23:45.678Z`)
- UUIDs: RFC 4122 UUID v4 with regex validation
- Language codes: ISO 639-2 3-letter codes (e.g., `kor`, `eng`, `jpn`)

These validations occur in `utils.c` using POSIX regex.

### Error Handling Strategy

**HTTP Status Code Mapping:**
- 400: Malformed JSON
- 422: Validation errors (invalid UUID, language code, timestamp)
- 500: Unexpected internal errors
- 502: OpenAI API 4xx errors (non-retryable)
- 503: OpenAI API 5xx/timeout (retryable, includes `Retry-After: 5` header)
- 504: Request timeout

**Retry Logic:**
- Only 5xx errors and timeouts are retried
- Maximum 3 attempts
- Exponential backoff: 2^attempt seconds
- UUID ensures idempotency

## Critical Implementation Details

### Language Code Handling

ISO 639-2 3-letter codes are converted to human-readable names in prompts:
- `kor` → "Korean"
- `eng` → "English"
- Mapping in `utils.c:get_language_name()`

This improves LLM translation quality vs raw codes.

### Prompt Template Processing

`sample_send.txt` has special handling:
- If contains `{{TEXT}}`, direct replacement
- If format is "... :: 번역요청 text 입니다.", text replaces after `::`
- Maintains backward compatibility with original template format

Implementation in `http_client.c:build_prompt()`.

### Response Preservation

Critical requirement: Response must preserve exact UUID and timestamp from request.
This enables client-side request/response correlation.

Implementation in `json_handler.c:create_translation_response()`.

### Emoji and Shortcode Stripping

Automatic removal of:
- Unicode emoji (U+1F300–U+1FAFF range)
- Shortcodes like `:smile:`, `:+1:`
- Extra whitespace normalization

Implementation in `utils.c:strip_emoji_and_shortcodes()`:
- UTF-8 character decoding
- Codepoint range checking
- Whitespace collapsing

### Hard Limits (hardcoded, not configurable)

Verified against the code and measured end-to-end on 2026-08-21. Public-facing copies of these numbers live in `README.md` → "한계와 보장"; keep both in sync when a constant changes.

| Constant | File | Value | Behaviour at the limit |
|---|---|---|---|
| `MAX_TEXT_LENGTH` | `json_handler.c` | 10,000 bytes | request rejected with 422 (explicit) |
| `MIN_TEXT_LENGTH` | `json_handler.c` | 1 byte | 422 |
| `MAX_TRANSLATION_BUFFER` | `http_client.c` | 16,384 | `unescape_string()` stops at 16,383 bytes and still returns 0 (**silent truncation**) |
| `MAX_CLEANED_TEXT_BUFFER` | `http_client.c` | 8,192 | `strip_emoji_and_shortcodes()` stops at 8,191 bytes and returns 0 — **the effective response cap for `normal`**, silent, cached truncated |
| `MAX_STREAM_BUFFER` | `http_client.c` | 65,536 | stream mode stops accumulating; only a `LOG_DEBUG` (invisible in release builds) |
| `MCP_RBUF_MAX` | `mcp_client.c` | 4 MB | codex path **errors** ("codex MCP response too large" → 502) rather than truncating |
| MHD connection timeout | `http_server.c` | 120 s | connection closed |
| curl timeout (`normal`) | `http_client.c` | 60 s/attempt, 3 attempts, `2^attempt` backoff | 503/504 |

The codex path deliberately bypasses `unescape_string()`/`strip_emoji_and_shortcodes()`, so it has **no 8 KB cap** — measured: a 10,533-byte translation came back whole while the same length would have been cut to 8,191 in `normal` mode.

There is **no cap on the accumulated HTTP request body** (`handle_translate()` reallocs per chunk); only the parsed `text` field is length-checked. The accumulation also uses `strlen()` per chunk, so it is O(n²) in body size and stops at the first NUL byte.

### Known thread-safety caveats (accepted, documented)

Audited 2026-08-21; none of these can put another request's translation into a response (per-request isolation was verified with 1,200 concurrent mixed-text requests, keep-alive reuse, and concurrent live codex calls — zero mismatches).

- `get_current_timestamp()` (`utils.c`) uses non-reentrant `gmtime()`. It only feeds the `timestamp` field of **error** responses; successful responses echo the request's own timestamp. Concurrent errors can therefore carry a slightly-off timestamp. Fix would be `gmtime_r`.
- `get_log_timestamp()` uses `localtime()` for the same reason — affects log line timestamps only.
- Debug traces are named `<timestamp>_<uuid>.txt`; two in-flight requests sharing a uuid+timestamp overwrite each other's trace file.
- The cache key is `SHA-256("from|to|text")` and does **not** include model, prompt, or backend — instances with different `MODEL_TYPE`/prompt must not share a cache DB.
- Translation output is not reproducible: codex is non-deterministic, and prompt rules can change the result kind (see `PROMPT_PREFIX_translategemma.txt`'s ASCII echo rule, which was observed returning long pure-ASCII input verbatim instead of translating it).

## Library Dependencies

Required libraries (see `LIBS` in the Makefile):
- **libcurl**: HTTP client for OpenAI API calls
- **libmicrohttpd**: HTTP server
- **cJSON**: JSON parsing and generation
- **libuuid**: UUID v4 generation
- **libm**: Math library for exponential backoff
- **OpenSSL** (`libssl`/`libcrypto`): SHA-256 cache keys (EVP API)
- **SQLite3**: `sqlite` cache backend

Install on Ubuntu/Debian:
```bash
sudo apt-get install libcurl4-openssl-dev libmicrohttpd-dev libcjson-dev \
                     uuid-dev libssl-dev libsqlite3-dev
```

## Environment Variables

- `TRANSBASKET_CONFIG`: Config file path (default: ../transbasket.conf)
- `MAX_WORKERS`: Thread pool size (default: 30)

Rule of thumb for `MAX_WORKERS`: `2 * CPU_CORES + 1`

## Development Patterns

### Adding New Language Support

Add to `ISO_639_2_CODES` array in `utils.c` and `LANGUAGE_NAMES` array in `get_language_name()`.

### Modifying OpenAI Retry Logic

Edit `http_client.c`:
- `max_retries` parameter in `openai_translator_init()`
- Backoff calculation: `pow(2, attempt)`
- Retryable condition: `http_code >= 500`

### Adding New Endpoints

In `http_server.c`:
1. Add route check in `request_handler()` function
2. Create handler function (e.g., `handle_translate()`)
3. Use `create_json_response()` for responses
4. Follow error response pattern with `create_error_response()`

### Memory Management

Critical rules:
- Every `malloc`/`strdup` must have corresponding `free`
- Use `calloc()` for zero-initialized memory
- Free cJSON objects with `cJSON_Delete()`
- Free curl responses with dedicated free functions
- Check all return values for NULL

### Testing Strategy

Test commands:
```bash
# Health check
curl http://localhost:8889/health

# Valid translation
curl -X POST http://localhost:8889/translate -H "Content-Type: application/json" -d '{...}'

# Invalid UUID (should return 422)
curl -X POST http://localhost:8889/translate -H "Content-Type: application/json" -d '{"uuid":"invalid",...}'

# Invalid language code (should return 422)
curl -X POST http://localhost:8889/translate -H "Content-Type: application/json" -d '{"from":"xyz",...}'
```

## Common Issues

### Compilation Errors

**Missing libraries**:
```bash
make check-deps  # Check which libraries are missing
sudo apt-get install <missing-lib>-dev
```

**Undefined reference to `pow`**:
Add `-lm` to LIBS in Makefile (already included).

**Do not drop `-D_DEFAULT_SOURCE` from CFLAGS**:
glibc hides `realpath()` behind `__USE_MISC`/`__USE_XOPEN_EXTENDED`, which `_POSIX_C_SOURCE=200809L` alone does not set. At `-O2` the fortify headers happen to declare it, so release builds look fine while `make debug` (`-O0`) only gets an implicit declaration - the returned pointer is truncated to `int` and the server crashes on startup while loading the config.

### Runtime Errors

**Config Not Found**:
- Use absolute paths with `-c` and `-p` options
- Or ensure executable is run from project root
- Check `TRANSBASKET_CONFIG` environment variable

**Port Already in Use**:
```bash
lsof -i :8889  # Find process using port
kill <PID>     # Kill it
```

**OpenAI API Connection**:
- Verify `OPENAI_BASE_URL` is accessible
- Test with: `curl $OPENAI_BASE_URL/v1/models`
- Check network connectivity

### Memory Leaks

Use valgrind for debugging:
```bash
valgrind --leak-check=full --show-leak-kinds=all ./transbasket
```

## Performance Characteristics

### vs Python POC

| Metric | Python POC | C Implementation |
|--------|-----------|------------------|
| Memory Usage | ~50 MB | ~5 MB |
| Startup Time | ~1 second | ~0.1 second |
| Binary Size | N/A | ~120 KB |
| Request Latency | Same (network bound) | Same (network bound) |

### Scalability

- Thread-per-connection model scales to hundreds of concurrent requests
- Limited by MAX_WORKERS setting
- Memory per thread: ~8 MB (stack size)
- Recommended MAX_WORKERS: 2 * CPU_CORES + 1

## Deployment

### Systemd Service

Create `/etc/systemd/system/transbasket.service`:
```ini
[Unit]
Description=Transbasket Translation Service
After=network.target

[Service]
Type=simple
User=transbasket
WorkingDirectory=/opt/transbasket
ExecStart=/opt/transbasket/transbasket -c /etc/transbasket/transbasket.conf -p /etc/transbasket/PROMPT_PREFIX.txt
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable and start:
```bash
sudo systemctl daemon-reload
sudo systemctl enable transbasket
sudo systemctl start transbasket
```

### Docker

See `Dockerfile` for containerized deployment (if available).

## Security Considerations

- Never commit API keys to repository
- Use environment variables or secure config files for `OPENAI_API_KEY`
- Run as non-root user in production
- Set appropriate file permissions on config files (600)
- Consider firewall rules to restrict port 8889 access
