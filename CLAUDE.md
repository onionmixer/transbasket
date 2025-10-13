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
./bin/transbasket

# With custom configuration
./bin/transbasket -c /path/to/config.conf -p /path/to/PROMPT_PREFIX.txt

# With custom worker count
./bin/transbasket -w 20

# Or with environment variable
MAX_WORKERS=20 ./bin/transbasket
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

The C implementation follows a clear separation of concerns across 6 modules:

1. **main.c**: Entry point with signal handling (SIGINT, SIGTERM, SIGHUP)
2. **config_loader.c**: Parses `transbasket.conf` (KEY="value" format) and `PROMPT_PREFIX.txt`
3. **utils.c**: Validation utilities (RFC 3339 timestamps, UUID v4, ISO 639-2 language codes, emoji stripping)
4. **json_handler.c**: JSON request/response handling using cJSON
5. **http_client.c**: OpenAI API client using libcurl with retry logic and exponential backoff
6. **http_server.c**: HTTP server using libmicrohttpd with thread-per-connection model

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

### Request Flow

1. Client → libmicrohttpd creates new thread
2. Thread → Validates JSON with cJSON
3. Thread → Loads prompt template from `sample_send.txt`
4. Thread → Builds prompt replacing `{{PROMPT_PREFIX}}`, `{{LANGUAGE_BASE}}`, `{{LANGUAGE_TO}}`
5. Thread → Calls OpenAI API via libcurl (with retry logic: max 3 attempts, exponential backoff)
6. Thread → Strips emoji/shortcodes from response
7. Thread → Sends JSON response preserving original UUID/timestamp
8. Connection closes, thread terminates

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

## Library Dependencies

Required libraries:
- **libcurl**: HTTP client for OpenAI API calls
- **libmicrohttpd**: HTTP server
- **cJSON**: JSON parsing and generation
- **libuuid**: UUID v4 generation
- **libm**: Math library for exponential backoff

Install on Ubuntu/Debian:
```bash
sudo apt-get install libcurl4-openssl-dev libmicrohttpd-dev libcjson-dev uuid-dev
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
valgrind --leak-check=full --show-leak-kinds=all ./bin/transbasket
```

## Performance Characteristics

### vs Python POC

| Metric | Python POC | C Implementation |
|--------|-----------|------------------|
| Memory Usage | ~50 MB | ~5 MB |
| Startup Time | ~1 second | ~0.1 second |
| Binary Size | N/A | ~50 KB |
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
ExecStart=/opt/transbasket/bin/transbasket -c /etc/transbasket/transbasket.conf -p /etc/transbasket/PROMPT_PREFIX.txt
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
