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
5. **http_client.c**: Translation dispatch — `normal` (OpenAI API via libcurl, retry/backoff) and `codex` backends
6. **http_server.c**: HTTP server using libmicrohttpd with thread-per-connection model
7. **codex_exec.c**: `codex exec` driver — the codex backend's default interface, plus the startup CLI probe
8. **mcp_client.c**: Minimal MCP stdio JSON-RPC 2.0 client for the legacy `codex mcp-server` interface (codex-cli < 0.154.0)
9. **source_hint.c**: script classifier for the from=auto base language (see "Source hint" below)
10. **strict_json.c**: strict validator for codex structured output, run before cJSON
11. **listen_address.c**: `LISTEN` parser and LISTEN line recognizer (see "LISTEN address" below)

(Cache and language-table concerns live in `trans_cache.c`, `cache_backend_*.c`, and `language_table.c`.)

### Threading Model

**Critical architectural pattern: Thread-per-Connection**

- **libmicrohttpd**: Creates a new thread for each incoming connection
- **Blocking I/O**: Each thread can safely block on OpenAI API calls
- **Connection Persistence**: HTTP connection maintained until translation completes
- **Scalability**: libmicrohttpd spawns one thread per connection

This approach is efficient because:
- No async complexity needed (blocking calls are fine in dedicated threads)
- Simple to reason about and debug

⚠ **`MAX_WORKERS` does not limit concurrent connections.** `MHD_start_daemon()` is called without
`MHD_OPTION_CONNECTION_LIMIT` or `MHD_OPTION_THREAD_POOL_SIZE` (`http_server.c`), so the connection
count is bounded by libmicrohttpd's own default, not by this setting. Since request coalescing,
`MAX_WORKERS` supplies the defaults for both `INFLIGHT_MAX_KEYS` and `CODEX_MAX_CONCURRENCY`.
Distinct active translations are limited by `INFLIGHT_MAX_KEYS`; same-key followers consume separate
waiter slots and connection threads. The earlier 500-request measurements predate these limits.

Implementation in `http_server.c`:
- `MHD_USE_THREAD_PER_CONNECTION` flag creates dedicated threads
- `openai_translate()` called directly (no executor needed)

Only a group's owner invokes the engine and, in codex mode, obtains a semaphore slot and spawns a child.
Followers block on a monotonic condition variable without starting their own engine invocation.

### Request Flow

1. Client → libmicrohttpd creates new thread
2. Thread → Validates JSON with cJSON (invalid → 422)
3. Thread → Cache lookup; on hit above threshold, returns cached translation
4. Cache miss or below threshold → join/register a shared translation group, rechecking cache before registration
5. Owner → `openai_translate()` dispatches on `MODEL_TYPE` with a shared deadline/cancellation context:
   - **normal**: builds prompt, calls OpenAI API via libcurl (retry: max 3, exponential backoff), unescapes + strips emoji/shortcodes
   - **codex**: builds a delimiter-isolated prompt, acquires a concurrency slot, then either runs `codex exec` with the prompt on stdin and reads `--output-last-message` (default), or spawns `codex mcp-server` and runs initialize + `tools/call` (legacy); trim + whole-fence unwrap only
6. Owner → updates cache once, publishes result/error, broadcasts and detaches group
7. Each participant → creates its own response preserving its UUID/timestamp, then releases its reference
8. Last reference → frees the detached group and result; connection thread returns

### Shared translation lifetime

`inflight_translation.c` owns the registry, mutex, monotonic conditions and reference counts.
`RUNNING` entries have registry/owner/follower references. Only the owner publishes and removes a key;
timed-out followers never promote themselves. Detached results remain immutable until the last reference drops.
Manager -> cache is the only nested lock order. Engine I/O, backoff and response creation hold neither lock.
The manager is shut down before `MHD_stop_daemon()` and destroyed only after all MHD handlers return.
Signal handlers set `sig_atomic_t` flags only; logging, SIGHUP cache saving and shutdown run in the main loop.

`TRANS_CACHE_THRESHOLD` still controls cache reuse. Followers do not update the cache count, so sharing
one model result among ten requests is one verification toward the threshold. Cache-hit usage counting remains.
Same-key matching uses normalized from/to and cleaned text, excluding UUID/timestamp, within one process only.

Configuration: `TRANSLATION_TOTAL_TIMEOUT=110` (1..110 seconds), `INFLIGHT_MAX_KEYS` defaults to
`MAX_WORKERS`, `INFLIGHT_MAX_WAITERS=256`, `INFLIGHT_MAX_WAITERS_PER_KEY=64`. Explicit limits must
be positive integers. Capacity rejection is 503 with Retry-After; cache hits bypass admission.
The total budget spans retries, backoff, slots, MCP initialization and calls. Normal total timeout is 503;
codex total timeout is 504, with shutdown mapped to 503. Non-streaming missing/empty normal replies now
fail with 502 instead of being cached as an empty string or the old `nothing contents` sentinel.

Run `make test-coalescing` for unit and local HTTP/exec/MCP checks. Sanitizer examples:

```bash
make test-inflight-unit INFLIGHT_TEST_BIN=obj/test_inflight_asan TEST_SANITIZERS='-fsanitize=address,undefined -fno-omit-frame-pointer -g'
make test-inflight-unit INFLIGHT_TEST_BIN=obj/test_inflight_tsan TEST_SANITIZERS='-fsanitize=thread -fno-omit-frame-pointer -g'
```

### Configuration System

**Policy: tracked `*.conf` files are templates, real values live in untracked `*.local.conf`.**

Anything carrying a real endpoint, API key, or internal host must not be committed. `load_config()`
therefore picks the config in this order when neither `-c` nor `TRANSBASKET_CONFIG` is given:

1. `transbasket.local.conf` next to the executable, if it exists (untracked, `.gitignore` covers `*.local.conf`)
2. `transbasket.conf` (the tracked template with placeholder values)

The startup log prints `Using configuration file: <path>` so the effective file is never in doubt.
Per-environment deployment configs (`transbasket_dkfs_codex.conf`, `transbasket_pge4_codex.conf`) are
untracked for the same reason. When editing a tracked template, keep placeholders such as
`http://<ollama-host>:11434/v1` and `OPENAI_API_KEY="."`.

Two configuration files:

1. **transbasket.conf**: Shell-style KEY="value" format (not INI)
   - Parsed with regex pattern matching
   - Custom validation for URLs, ports
   - The config file itself is resolved relative to the executable; prompt, role and language
     table paths inside it are resolved relative to the **config file's directory**

2. **PROMPT_PREFIX.txt**: Multi-line prompt prefix
   - Loaded at startup
   - Supports complex instructions
   - UTF-8 encoded

### Translation Backend (MODEL_TYPE)

`MODEL_TYPE` selects how translations are produced (see `config_loader.c`, `http_client.c:openai_translate()` dispatch):

- **`normal`** (default): existing OpenAI-compatible HTTP API path (`/v1/chat/completions` via libcurl). Uses `OPENAI_BASE_URL/MODEL/API_KEY`. Behavior unchanged.
- **`codex`**: drives the installed `codex` CLI. `OPENAI_*` are unused; `CODEX_*` settings apply. `CODEX_INTERFACE` picks how:
  - **`exec`** (default, `codex_exec.c`): `codex exec --skip-git-repo-check --color never [--ephemeral] [--ignore-user-config] -s … -c approval_policy="…" [-c model_reasoning_effort="…"] [-m …] -C <tmp> -o <tmp>/last-message.txt -`
  - **`mcp`** (legacy, `mcp_client.c`): `codex mcp-server` over stdio JSON-RPC 2.0, result from `structuredContent.content`

⚠ **codex-cli 0.154.0 removed `codex mcp-server`** (0 occurrences in the 0.166.0 binary). A current CLI parses it as
an interactive prompt and exits with `Error: stdin is not a terminal`; with `CODEX_INTERFACE="mcp"` every request
then fails with `502 codex MCP initialize failed`. Keep `exec` unless the host is pinned to an older CLI. The codex
CLI surface keeps moving, which is why `openai_translator_init()` runs `codex_cli_probe()` at startup (version,
`exec --help` flag detection, `mcp list --json`) and logs a `Warning:` when the chosen interface or flag is missing.

codex backend specifics:
- One codex child is spawned per request (`posix_spawnp`, in its own process group so the node wrapper **and** the real codex grandchild are reaped on stop). Note codex itself starts MCP-server helpers in *their own* process groups, so a timeout kill does not reach them; codex reaps them when it exits.
- exec: the prompt goes on **stdin, never argv** (argv is world-readable via `ps`/`/proc/<pid>/cmdline`). codex echoes the full prompt on **stderr**, so stderr must never be copied into client-facing errors — only the last `ERROR:` line is (`last_error_line()`); the full tail goes to the DEBUG trace only.
- exec: `CODEX_APPROVAL_POLICY` / `CODEX_REASONING_EFFORT` become TOML `-c key="value"` overrides, so `validate_config()` restricts them to `[A-Za-z0-9._-]{1,64}`.
- exec: `--ephemeral` is passed only if `codex exec --help` advertises it (keeps `$CODEX_HOME/sessions` from growing).
- **MCP servers / plugins from the codex user's `config.toml` start on every translation** and their tools are not covered by `sandbox=read-only`, so untrusted text could steer codex into calling them (measured: 5 configured servers → 9 helper processes per request). `CODEX_IGNORE_USER_CONFIG=true` (exec only) passes `--ignore-user-config`, which also drops the config's `model`, so `CODEX_MODEL` must be set — codex's built-in default model rejected `model_reasoning_effort="none"`. Default is `false` so existing deployments keep working; startup logs a warning listing the servers.
- Concurrency is capped by a semaphore in `OpenAITranslator` (`CODEX_MAX_CONCURRENCY`, default = `MAX_WORKERS`); exhaustion → 503, call timeout → 504.
- Untrusted text is isolated with a per-request random delimiter (uuid-based); `sandbox=read-only`/`approval-policy=never` are force-validated unless `CODEX_ALLOW_UNSAFE=true`.
- Each request runs in a fresh `mkdtemp` cwd under `CODEX_RUN_DIR` (default `/var/lib/transbasket/run`), removed afterward.
- codex output is **not** re-unescaped or emoji-stripped (would corrupt it), only trimmed + whole-output code-fence unwrapped.
- Requires the host (service user) to have `codex login` done and a writable `CODEX_HOME`. See `PLAN_MODEL_TYPE_CODEX_MCP.md` §7.1 for the systemd/operator checklist (that plan describes the now-legacy MCP route; the operator steps still apply).
- `mcp_client.c:wait_for_id()` must consume the matched line from `rbuf` before returning. It used to leave it there with its `\n` already turned into `\0`, so the next wait parsed the stale line and swallowed the following one; with the real `codex mcp-server` that was a `codex/event` notification, so it only failed against servers that answer without notifications (fixed 2026-10-31).
- **`TRANS_CACHE_THRESHOLD` must be `1` in codex mode.** codex output is non-deterministic, so the same source text comes back slightly reworded on each call; `handle_translate()` then treats it as a changed translation and resets the entry's `count` to 1. With any threshold above 1 the count never reaches it, the cache never serves a hit, and every request pays for a codex call. Verified: threshold 3 → 4 identical requests all took ~4 s and re-invoked codex; threshold 1 → second request served from cache in ~0.01 s.

### Source hint (from=auto base language) - PLAN_SOURCE_HINT_METADATA.md

`from=auto` results carry a `source_hint` (canonical code or `und`, `mixed`, `origin` model|script,
`hint_version`). Invariants that must not be broken:

- **The cache key and the HTTP response do not change.** The hint lives beside the row.
- SQLite: side table `trans_cache_source_hint` + AFTER DELETE trigger + owned code table
  `trans_cache_hint_code` (filled from `ISO_639_2_CODES` + `und`) + view `trans_cache_effective_source`.
  `user_version` stays 2 so a binary without hints still opens the DB (measured). The trigger and view
  are re-created on every start **after** the v0/v1 migration (its DROP TABLE drops triggers without firing them).
- The owner writes result + hint through `trans_cache_store_result()`: one lock, deadline checked after
  the lock, BEGIN IMMEDIATE -> parent -> SAVEPOINT hint -> COMMIT. A hint error never loses the parent;
  a transaction-aborting one retries the parent alone; SQLITE_BUSY fails fast; the caller's copy changes
  only after commit. Priority: model > script, then version.
- `SOURCE_HINT_MODE=model` (codex exec only): `--output-schema`, the `-o` file is the only result, and the
  reply passes `strict_json_validate()` **before** cJSON - cJSON 1.7.15 decodes invalid `\u` escapes to NUL,
  accepts control bytes/raw LF/`01`/BOM and its default lookup ignores case. Structural violations -> 502,
  nothing cached; semantic hint errors -> keep the translation, use the script classifier.
- Plain-mode prompts (explicit requests, auto with script mode, MCP, normal) are byte-identical to the
  pre-hint binary: `python3 tests/test_prompt_regression.py <old> <new>`.
- C <-> spec differential tests: `make test-source-hint` (classifier vs `tests/source_hint_ref.py`, table
  generated by `tests/source_hint_gen.py --c`) and `make test-strict-json` (vs `tests/strict_json_ref.py`).
  Change a reference and its C code together; a classifier rule change needs a new `SOURCE_HINT_VERSION`.
- `TRANSBASKET_TEST_HOOKS` exists only in the `make test-structured-hint` test binary.

### cJSON NUL guard and cleanup days - PLAN_CLEANUP_DAYS_AND_NUL.md

cJSON 1.7.15 decodes `\u0000` **and every `\u` whose next four bytes are not all hex digits** to U+0000,
and the C string is cut there (keys too). Invariants:

- Every parse of a request or a translation-carrying reply is `cJSON_ParseWithOpts(x, &end, 0)` (same
  behaviour as `cJSON_Parse`) followed by `json_decodes_nul(x, end - x)` - **only the consumed bytes**.
  Checking the whole buffer would newly reject SSE comments, ignored chunks and bytes after the root.
- Request -> 422 before any engine call. MCP: the flag is computed on the matched line **before** the
  `memmove` in `wait_for_id()`. SSE: a NUL chunk fails the whole response; the other `NULL` returns of
  `parse_sse_chunk()` stay ignored. Plain codex output: `memchr(result, 0, result_len)`. All 502, never cached.
- Not guarded on purpose: the text-cache loader (only our own C strings are written) and the startup
  `mcp list --json` probe (server names for a log line).
- `make test-cjson-nul`: spec `tests/cjson_nul_ref.py` vs the real libcjson through an allocator-hook
  oracle, and C vs spec. `make test-nul-guard`: every path through the server with offline fakes.
- Tool inputs and editors may decode `\uXXXX` sequences when writing test files: the spec tables use
  `~` for a backslash, and written files should be checked for decoded characters.
- Cleanup cutoffs: `trans_cache_cutoff()` / `trans_cache_cleanup_interval()` (64-bit, saturated). Never
  multiply days in `int` again. `TRANS_CACHE_CLEANUP_DAYS` keeps atoi's value where atoi is defined and
  saturates otherwise; `cache_tool cleanup/delete` take strict decimals. `make test-cleanup-days`.

### LISTEN address - PLAN_LISTEN_ADDRESS.md

Until 2026-11-06 `MHD_start_daemon()` got only the port, so every `LISTEN` value bound `0.0.0.0`. Invariants now:

- `parse_listen_address()` is the only parser, used by `validate_config()` and `translation_server_start()`.
  Numeric `inet_pton` literals only, no DNS (`localhost` resolves to `::1` first on this host). `::` means
  `MHD_USE_DUAL_STACK`; other IPv6 `MHD_USE_IPv6`. Multicast, `255.255.255.255`, `::ffff:0:0/96` and
  `fe80::/10` are rejected; anything else that cannot be bound fails at bind time with the MHD reason logged.
- `MHD_OPTION_SOCK_ADDR` decides the bind; the `port` argument is ignored for binding but appears in MHD's
  error text, so it gets the same value. `MHD_OPTION_EXTERNAL_LOGGER` must be the first option.
  MHD messages are INFO only until `MHD_start_daemon()` returns, then DEBUG (never used as a format string).
- A config line that tries to set LISTEN (`listen_scan_*`: blanks/BOM, optional `export`, the word LISTEN in
  any case) but is not read as `LISTEN=<value>` - parse failure, overlong value, overlong physical line -
  stops startup instead of leaving the `0.0.0.0` default. Note POSIX ERE: `LISTEN=` is an empty assignment.
- The log line `HTTP server listening on ...` comes from `getsockname()` + `IPV6_V6ONLY`, not from the config.
- `make test-listen-parse` (C vs `tests/listen_address_ref.py`, real `parse_config_line()`, mutants) and
  `make test-listen-address` (real server inside `unshare -rn --pid --fork --kill-child=KILL --mount-proc`
  with a dummy 198.18.0.1 interface; `LISTEN_OLD_BIN=` also runs a pre-fix binary). Tests bind loopback only.
- `-d` still daemonizes before the config is read, so startup errors are invisible there (separate task).

### Translation Cache Concurrency

The cache is shared by every connection thread, so two rules are load-bearing:

- **All backend calls are serialized.** `trans_cache_lookup()/_save()/_stats()` take the *write* lock even though they read: the sqlite backend drives one shared `sqlite3_stmt` (reset/bind/step/column) and the text backend refreshes `last_used`. Running them concurrently under a read lock corrupts the statement — `sqlite3_column_text()` returns NULL and the daemon segfaults in `strncpy`. Do not "optimize" these back to `rdlock`.
- **`trans_cache_lookup()` returns a caller-owned deep copy**, in every backend. Release it with `trans_cache_entry_free()`; never hand a stored entry outside the backend, or another thread can free its strings mid-read. `update_count()`/`update_translation()` locate the stored record via `entry->hash` and keep the caller's copy in sync.

Text-backend `add()` skips insertion when the hash is already stored (two threads can miss on the same new text at once); the sqlite backend gets this from its `UNIQUE` index.

### Validation Standards

**Strictly enforced formats:**
- Timestamps: RFC 3339 (e.g., `2025-10-10T01:23:45.678Z`)
- UUIDs: RFC 4122 UUID v4 with regex validation
- Language codes: ISO 639-2 3-letter codes (e.g., `kor`, `eng`, `jpn`), English language names,
  and Chinese BCP-47 tags (`zh-Hans`/`zh-Hant`, see "Chinese Script Codes")
- Source-only mode: case-insensitive `auto`, canonicalized before general language validation;
  target `auto`, omitted source and surrounding whitespace are rejected with 422

Timestamp/UUID validation uses POSIX regex in `utils.c`. Language validation uses
the supported-code/name lists and Chinese tag parser, with a separate source-mode check.

### Error Handling Strategy

**HTTP Status Code Mapping:**
- 422: JSON parsing failures and validation errors (invalid UUID, language code, timestamp or text,
  or an escape that cJSON would decode to U+0000 - see "cJSON NUL guard");
  `handle_translate()` maps every failed `parse_translation_request()` to this status
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

`from=auto` is a source mode, handled by `normalize_source_language()` BEFORE
general normalization or language table lookup. Do not add it to the table or
`validate_language_code()`; `to=auto` must stay invalid. Source buffers use
`SOURCE_LANGUAGE_SIZE` (5, including NUL); target buffers remain 4 bytes.
The auto branch in `build_instruction_message()` resolves only the target and
uses its own built-in policy plus optional `PROMPT_PREFIX_AUTO_FILE` content.
Source placeholders in that override are rejected at startup. Explicit-language
requests retain the existing template path. Auto cache keys use literal `auto`,
separate from detected/explicit languages. SQLite startup migrates known version
0/1 schemas atomically to version 2, preserving rows and auxiliary objects.
Never reuse an auto cache with an old binary (it truncates source codes to 3 chars).
Run `make test-auto` for offline API, prompt, cache and migration coverage.
The 2026-11-03 run passed 9 tests using fake HTTP/exec/MCP endpoints. The live
TranslateGemma attempt returned 503; live auto translation quality (including
real codex) remains unverified. See `TEST_RESULTS.md` for scope and results.

ISO 639-2 3-letter codes are converted to human-readable names in prompts:
- `kor` → "Korean"
- `eng` → "English"
- Mapping in `utils.c:get_language_name()`

This improves LLM translation quality vs raw codes.

### Prompt Template Processing

`http_client.c:build_instruction_message()` builds the instruction from
`PROMPT_PREFIX_FILE`, and the template's own content selects the format:

- **contains `{TEXT}`** (TranslateGemma style, e.g. `PROMPT_PREFIX_translategemma.txt`):
  `{SOURCE_LANG}`, `{SOURCE_CODE}`, `{TARGET_LANG}`, `{TARGET_CODE}` and `{TEXT}` are substituted
  from the language table, and everything travels as a single user message
- **no `{TEXT}`** (legacy, e.g. `PROMPT_PREFIX.txt`): only the target language is substituted, into
  `[TARGET LANGUAGE]` or `{{LANGUAGE_TO}}`, and the text is sent as a separate message

The codex backend reuses the same instruction but appends the source inside a per-request random
delimiter instead of substituting it blindly.

### Response Preservation

Critical requirement: Response must preserve exact UUID and timestamp from request.
This enables client-side request/response correlation.

Implementation in `json_handler.c:create_translation_response()`.

### Emoji and Shortcode Stripping

**Normal backend only.** `strip_emoji_and_shortcodes()` runs in `translate_via_openai()` and nowhere
else; the codex path deliberately skips it (re-processing already-decoded output would corrupt it) and
only trims plus unwraps a whole-output code fence. Note this is also where normal-backend responses get
truncated at 8,191 bytes - see "Hard Limits".

Automatic removal of:
- Unicode emoji (U+1F300–U+1FAFF range)
- Shortcodes like `:smile:`, `:+1:`
- Extra whitespace normalization

Implementation in `utils.c:strip_emoji_and_shortcodes()`:
- UTF-8 character decoding
- Codepoint range checking
- Whitespace collapsing

### Size limits and request budgets

Size measurements date from 2026-08-21; request budgets reflect the 2026-11-03 coalescing change.
Public-facing copies live in `README.md` → "한계와 보장"; keep both in sync.

| Constant | File | Value | Behaviour at the limit |
|---|---|---|---|
| `MAX_TEXT_LENGTH` | `json_handler.c` | 10,000 bytes | request rejected with 422 (explicit) |
| `MIN_TEXT_LENGTH` | `json_handler.c` | 1 byte | 422 |
| `MAX_TRANSLATION_BUFFER` | `http_client.c` | 16,384 | `unescape_string()` stops at 16,383 bytes and still returns 0 (**silent truncation**) |
| `MAX_CLEANED_TEXT_BUFFER` | `http_client.c` | 8,192 | `strip_emoji_and_shortcodes()` stops at 8,191 bytes and returns 0 — **the effective response cap for `normal`**, silent, cached truncated |
| `MAX_STREAM_BUFFER` | `http_client.c` | 65,536 | stream mode stops accumulating; only a `LOG_DEBUG` (invisible in release builds) |
| `CODEX_OUT_MAX` | `codex_exec.c` | 4 MB | exec path **errors** ("codex response too large" → 502) rather than truncating |
| `MCP_RBUF_MAX` | `mcp_client.c` | 4 MB | legacy MCP path **errors** ("codex MCP response too large" → 502) rather than truncating |
| MHD connection timeout | `http_server.c` | 120 s | connection closed |
| curl timeout (`normal`) | `http_client.c` | min(60 s, remaining total budget), up to 3 attempts | 503 for timeout |
| Total translation budget | `TRANSLATION_TOTAL_TIMEOUT` | default 110 s, configurable 1..110 | normal 503, codex 504; no follower deadline extension |

The codex path deliberately bypasses `unescape_string()`/`strip_emoji_and_shortcodes()`, so it has **no 8 KB cap** — measured: a 10,533-byte translation came back whole while the same length would have been cut to 8,191 in `normal` mode.

There is **no cap on the accumulated HTTP request body** (`handle_translate()` reallocs per chunk); only the parsed `text` field is length-checked. The accumulation also uses `strlen()` per chunk, so it is O(n²) in body size and stops at the first NUL byte.

### Known thread-safety caveats (accepted, documented)

Audited 2026-08-21; none of these can put another request's translation into a response (per-request isolation was verified with 1,200 concurrent mixed-text requests, keep-alive reuse, and concurrent live codex calls — zero mismatches).

- Timestamp formatting uses `gmtime_r`/`localtime_r` to avoid shared calendar buffers under concurrent completions.
- Debug traces are named `<timestamp>_<uuid>.txt`; two in-flight requests sharing a uuid+timestamp overwrite each other's trace file.
- The cache key is `SHA-256("from|to|text")` and does **not** include model, prompt, or backend — instances with different `MODEL_TYPE`/prompt must not share a cache DB.
- Translation output is not reproducible: codex is non-deterministic, and explicit-language prompt rules can change the result kind (see `PROMPT_PREFIX_translategemma.txt`'s ASCII echo rule). Source `auto` uses its separate policy and optional override, not that template. Both still use the existing backend-specific output processing.

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

- `TRANSBASKET_CONFIG`: Config file path. Unset means `transbasket.local.conf` next to the
  executable if present, otherwise `transbasket.conf`
- `MAX_WORKERS`: Thread pool size (default: 30)

Rule of thumb for `MAX_WORKERS`: `2 * CPU_CORES + 1`

## Development Patterns

### Adding New Language Support

Add to `ISO_639_2_CODES` array in `utils.c` and `LANGUAGE_NAMES` array in `get_language_name()`.

Both arrays matter: `ISO_639_2_CODES` decides what requests are accepted, and `LANGUAGE_NAMES` supplies
the name that reaches the prompt. A code present in the first but missing from the second falls back to
the raw code in the prompt, which the model cannot interpret.

### Chinese Script Codes (`zhs` / `zht`)

Clients send BCP-47 tags (`zh-Hans`, `zh-Hant`, `zh-CN`, `zh-TW`, …), and
`normalize_language_code()` folds Chinese tags into three-character transbasket codes.
Targets still use 4-byte buffers and `CHECK(length(to_lang) = 3)`. Since the auto change,
source buffers use `SOURCE_LANGUAGE_SIZE` (5 bytes), and SQLite version 2 accepts
`length(from_lang) = 3 OR from_lang = 'auto'`. Chinese tags keep the same codes and hashes:

| Input | Internal code | Prompt language name |
|---|---|---|
| `zh-Hans*`, `zh-CN`, `zh-SG`, `zh-MY`, `zhs`, `Chinese (Simplified)` | `zhs` | Chinese (Simplified) |
| `zh-Hant*`, `zh-TW`, `zh-HK`, `zh-MO`, `zht`, `Chinese (Traditional)` | `zht` | Chinese (Traditional) |
| `zh`, `zho`, `chi`, `Chinese` | `chi` | Chinese |

Rules enforced in `normalize_chinese_tag()`:
- every subtag must be a non-empty `[a-z0-9]` run of at most 8 characters, so `zh-Hans-`,
  `zh-Hans--CN`, and `zh-Hans-中文` are rejected (422)
- a script subtag (`hans`/`hant`) outranks any region subtag, so `zh-CN-Hant` is Traditional
- `fold_lang_tag()` bounds the input at `LANG_TAG_MAX` and casts to `unsigned char` before `tolower()`

Two files must stay in sync — `src/utils.c` (`ISO_639_2_CODES`, `LANGUAGE_NAMES`) and
`language_tables.conf`. In the config the `zhs:zh`/`zht:zh` mappings must stay **after** `chi:zh`
(`language_table_to_iso639_2()` returns the first match), and the `zhs=`/`zht=` name entries must exist:
without them `language_table_resolve()` falls back to the name for `zh` and a Traditional request is
silently translated as Simplified. Full rationale in `PLAN_CHINESE_SCRIPT_CODES.md`.

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

- Thread-per-connection model scales to hundreds of concurrent requests (measured: 500 concurrent)
- Not limited by `MAX_WORKERS` - see the warning under "Threading Model"
- Memory per thread: ~8 MB of virtual address space (stack reservation)
- `MAX_WORKERS` still sets the default codex concurrency; `2 * CPU_CORES + 1` remains a sane value

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

There is no Dockerfile in this repository. Containerizing means installing the libraries from
"Library Dependencies", running `make`, and mounting the config, prompt, language table and cache
paths - remember that relative paths in the config resolve against the executable's directory.

## Security Considerations

- Never commit API keys to repository
- Use environment variables or secure config files for `OPENAI_API_KEY`
- Run as non-root user in production
- Set appropriate file permissions on config files (600)
- Consider firewall rules to restrict port 8889 access
