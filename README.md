# Transbasket C Implementation

Production-ready C implementation of the Transbasket translation server for optimal performance.

## Features

- High-performance HTTP server using libmicrohttpd
- OpenAI-compatible API client using libcurl
- JSON handling with cJSON
- Thread-per-connection model for concurrent requests
- Emoji and shortcode stripping
- RFC 3339 timestamp and UUID v4 validation
- Graceful shutdown with signal handling
- Retry logic with exponential backoff

## Requirements

### System Libraries

```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    libcurl4-openssl-dev \
    libmicrohttpd-dev \
    libcjson-dev \
    uuid-dev \
    pkg-config

# RHEL/CentOS/Fedora
sudo yum install -y \
    gcc make \
    libcurl-devel \
    libmicrohttpd-devel \
    cjson-devel \
    libuuid-devel \
    pkg-config

# macOS (Homebrew)
brew install \
    curl \
    libmicrohttpd \
    cjson \
    ossp-uuid
```

## Build

### Check Dependencies

```bash
make check-deps
```

### Compile

```bash
make
```

This will create the executable at `bin/transbasket`.

### Debug Build

```bash
make debug
```

## Configuration

The server requires two configuration files:

### 1. transbasket.conf

Located at `../transbasket.conf` (relative to executable):

```
OPENAI_BASE_URL="http://192.168.1.239:11434/v1"
OPENAI_MODEL="gpt-oss:20b"
OPENAI_API_KEY="."
LISTEN="0.0.0.0"
PORT="8889"
```

#### Translation backend: `MODEL_TYPE`

```
MODEL_TYPE="normal"   # normal (default) | codex
```

- **`normal`** — translate via the OpenAI-compatible HTTP API (`OPENAI_*`). Default; unchanged behavior.
- **`codex`** — translate by driving the installed `codex` CLI as an MCP server (`codex mcp-server`). `OPENAI_*` are ignored; the `CODEX_*` keys apply (see comments in `transbasket.conf`):

```
MODEL_TYPE="codex"
#CODEX_BIN="codex"              # codex executable (PATH-searched)
#CODEX_MODEL="gpt-5.2"          # optional model override (unset = codex default)
#CODEX_TIMEOUT="100"            # seconds (must be < MHD 120s)
#CODEX_MAX_CONCURRENCY="8"      # concurrent codex processes (default = MAX_WORKERS)
#CODEX_RUN_DIR="/var/lib/transbasket/run"   # writable root for per-request temp cwd
#CODEX_REASONING_EFFORT="none"  # optional arguments.config injection
#CODEX_ALLOW_UNSAFE="false"     # allow non read-only/never (dev only)
```

codex mode requires the running user to have completed `codex login` and to have a writable `CODEX_HOME`. The endpoint translates untrusted text, so `sandbox` is forced to `read-only` and `approval-policy` to `never` unless `CODEX_ALLOW_UNSAFE=true`. See `PLAN_MODEL_TYPE_CODEX_MCP.md` for design and the operator/systemd checklist.

### 2. PROMPT_PREFIX.txt

Located at `../PROMPT_PREFIX.txt` (relative to executable):

```
translation from language to language. not add more text. note add emoji syntax. only return plain translate text.
```

### 3. sample_send.txt (Optional)

Template file for translation prompts. Default format:

```
{{PROMPT_PREFIX}} FROM {{LANGUAGE_BASE}} to {{LANGUAGE_TO}} :: {{TEXT}}
```

### 4. SAMPLE_SOURCE_ENGLISH.txt (Test File)

번역 규칙 테스트를 위한 샘플 영문 텍스트 파일입니다. 다음 규칙들을 테스트합니다:

| 테스트 항목 | 설명 |
|-------------|------|
| 명령어 오인 방지 | `REVIEW`, `DELETE`, `UPDATE` 등 ASCII 단어가 명령으로 해석되지 않음 |
| 특수문자 보존 | `@#$%^&*()_+-=[]{}` 등 특수문자 유지 |
| 공백 보존 | 스페이스, 탭, 혼합 공백 유지 |
| 리터럴 `\n` 보존 | 문자열 내 `\n`이 줄바꿈으로 변환되지 않음 |
| 따옴표 보존 | 큰따옴표, 작은따옴표 유지 |
| 빈 줄 보존 | 연속된 빈 줄 유지 |
| 이모지 태그 변환 | 😀→[SMILE], 🌅→[SUN], 👍→[THUMBS_UP] |
| 코드 구문 보존 | `if (x > 0) { return true; }` 등 코드 형태 유지 |

**테스트 방법:**
```bash
# JSON payload 생성
SAMPLE_TEXT=$(cat SAMPLE_SOURCE_ENGLISH.txt)
JSON_PAYLOAD=$(jq -n \
  --arg ts "2026-01-25T12:30:00.000Z" \
  --arg uuid "990e8400-e29b-41d4-a716-446655440004" \
  --arg from "eng" \
  --arg to "kor" \
  --arg text "$SAMPLE_TEXT" \
  '{timestamp: $ts, uuid: $uuid, from: $from, to: $to, text: $text}')

# 번역 요청
curl -X POST http://localhost:8889/translate \
  -H "Content-Type: application/json" \
  -d "$JSON_PAYLOAD"
```

## Usage

### Basic Usage

```bash
./bin/transbasket
```

### With Custom Configuration

```bash
./bin/transbasket -c /path/to/config.conf -p /path/to/PROMPT_PREFIX.txt
```

### With Custom Worker Count

```bash
./bin/transbasket -w 20
```

or

```bash
MAX_WORKERS=20 ./bin/transbasket
```

### Command Line Options

```
Usage: transbasket [OPTIONS]

Options:
  -c, --config PATH       Path to configuration file (default: ../transbasket.conf)
  -p, --prompt PATH       Path to prompt prefix file (default: ../PROMPT_PREFIX.txt)
  -w, --workers NUM       Number of worker threads (default: 30)
  -h, --help              Show help message

Environment Variables:
  TRANSBASKET_CONFIG      Config file path
  MAX_WORKERS             Thread pool size
```

## API Endpoints

The server exposes two HTTP endpoints:

### GET /health

헬스체크 엔드포인트입니다. 서버의 상태를 확인합니다.

**Request:**
```bash
curl http://localhost:8889/health
```

**Response:**
```json
{
  "status": "healthy",
  "service": "transbasket",
  "version": "1.0.0",
  "model_type": "normal"
}
```

`model_type` reflects the active backend (`normal` or `codex`). The health check is shallow and does not spawn codex.

**Status Codes:**
- `200 OK`: 서버가 정상 작동 중

---

### POST /translate

번역 요청 엔드포인트입니다. 텍스트를 지정된 언어로 번역합니다.

**Request:**
```bash
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

**Request Body:**
- `timestamp` (string, required): RFC 3339 형식의 타임스탬프
- `uuid` (string, required): RFC 4122 UUID v4 형식의 고유 식별자
- `from` (string, required): 원본 언어 코드 (ISO 639-2, 3자리)
- `to` (string, required): 대상 언어 코드 (ISO 639-2, 3자리)
- `text` (string, required): 번역할 텍스트

**Supported Language Codes:**

번역 지원 언어는 Model에 따라 다르며, 언어의 형식은 `language_tables.conf`를 참고하세요.

**Response:**
```json
{
  "timestamp": "2025-10-10T01:23:45.678Z",
  "uuid": "550e8400-e29b-41d4-a716-446655440000",
  "translatedText": "Hello"
}
```

**Response Body:**
- `timestamp` (string): 요청의 타임스탬프 (원본 그대로 반환)
- `uuid` (string): 요청의 UUID (원본 그대로 반환)
- `translatedText` (string): 번역된 텍스트 (이모지 및 shortcode 제거됨)

**Status Codes:**
- `200 OK`: 번역 성공
- `400 Bad Request`: 잘못된 JSON 형식
- `422 Unprocessable Entity`: 유효하지 않은 UUID, 언어 코드, 또는 타임스탬프
- `500 Internal Server Error`: 예상치 못한 내부 오류
- `502 Bad Gateway`: OpenAI API 4xx 오류 (재시도 불가)
- `503 Service Unavailable`: OpenAI API 5xx/타임아웃 (재시도 가능, `Retry-After: 5` 헤더 포함)
- `504 Gateway Timeout`: 요청 타임아웃

## Project Structure

```
c-impl/
├── Makefile              # Build system
├── README.md             # This file
├── include/              # Header files
│   ├── utils.h
│   ├── config_loader.h
│   ├── json_handler.h
│   ├── http_client.h
│   ├── http_server.h
│   └── mcp_client.h      # codex MCP client
├── src/                  # Source files
│   ├── utils.c
│   ├── config_loader.c
│   ├── json_handler.c
│   ├── http_client.c
│   ├── http_server.c
│   ├── mcp_client.c      # codex MCP client (MODEL_TYPE=codex)
│   └── main.c
├── obj/                  # Object files (generated)
└── bin/                  # Executable (generated)
```

## Module Overview

### utils.c
- ISO 639-2 language code validation
- UUID v4 validation (RFC 4122)
- RFC 3339 timestamp validation
- Emoji and shortcode stripping
- Text truncation utilities

### config_loader.c
- Configuration file parsing (KEY="value" format)
- Prompt prefix file loading
- Configuration validation
- Path resolution

### json_handler.c
- JSON request parsing with cJSON
- JSON response generation
- Error response formatting
- Request/response validation

### http_client.c
- Translation dispatch on `MODEL_TYPE` (normal vs codex)
- normal: OpenAI API communication with libcurl, retry with exponential backoff
- codex: builds delimiter-isolated prompt, concurrency semaphore, per-request temp cwd
- Prompt template processing
- Error handling and status code mapping

### mcp_client.c
- Minimal MCP stdio JSON-RPC 2.0 client for `codex mcp-server`
- Spawns codex in its own process group (`posix_spawnp`), reaps the whole tree
- initialize handshake + `tools/call` (name="codex"), id-matched responses
- Drains stdout/stderr concurrently (deadlock-safe), EINTR-tolerant, bounded buffers

### http_server.c
- HTTP server with libmicrohttpd
- Thread-per-connection model
- Health check endpoint
- Translation endpoint
- Error response handling

### main.c
- Entry point with signal handling
- Command line argument parsing
- Logging and startup banner
- Graceful shutdown

## Installation

```bash
# Install system-wide
sudo make install

# Uninstall
sudo make uninstall
```

## Development

### Clean Build Artifacts

```bash
make clean
```

### Rebuild Everything

```bash
make rebuild
```

### Code Style

- C11 standard
- POSIX compliant
- No GNU extensions
- Clear error messages to stderr

## Performance Considerations

- Thread-per-connection model handles multiple concurrent requests
- Memory is carefully managed (no leaks)
- Retry logic prevents temporary failures
- Connection pooling in libcurl for efficiency

## Comparison with Python POC

| Feature | Python POC | C Implementation |
|---------|-----------|------------------|
| Performance | ~50 req/s | ~500+ req/s |
| Memory Usage | ~50 MB | ~5 MB |
| Startup Time | ~1 second | ~0.1 second |
| Threading | ThreadPoolExecutor | Thread-per-connection |
| API Contract | ✓ Identical | ✓ Identical |

## License

Same as parent project.

## Support

See parent project README and CLAUDE.md for architecture details.
