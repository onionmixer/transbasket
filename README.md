# Transbasket C Implementation

Production-ready C implementation of the Transbasket translation server for optimal performance.

## Features

- High-performance HTTP server using libmicrohttpd
- Pluggable translation backend via `MODEL_TYPE`:
  - **normal**: OpenAI-compatible API client using libcurl (default)
  - **codex**: drives the installed `codex` CLI as an MCP server (`codex mcp-server`)
- JSON handling with cJSON
- Thread-per-connection model for concurrent requests
- Translation cache (text / SQLite backends), keyed per language pair
- Chinese script support: `zh-Hans` / `zh-Hant` translate and cache separately
- Emoji and shortcode stripping (normal backend only - codex output is passed through)
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
    libssl-dev \
    libsqlite3-dev \
    pkg-config

# RHEL/CentOS/Fedora
sudo yum install -y \
    gcc make \
    libcurl-devel \
    libmicrohttpd-devel \
    cjson-devel \
    libuuid-devel \
    openssl-devel \
    sqlite-devel \
    pkg-config

# macOS (Homebrew)
brew install \
    curl \
    libmicrohttpd \
    cjson \
    ossp-uuid \
    openssl \
    sqlite
```

### Optional: codex CLI (only for `MODEL_TYPE=codex`)

The `codex` translation backend shells out to the installed Codex CLI as an MCP
server. It is **not** needed for the default `normal` backend.

```bash
# Verify codex is installed and on PATH
codex --version

# The user/account that RUNS transbasket must be logged in, with a writable CODEX_HOME
codex login status
```

When running as a service user, ensure that user has its own `codex login` and a
writable `CODEX_HOME` (see the operator checklist in `PLAN_MODEL_TYPE_CODEX_MCP.md`).

## Build

### Check Dependencies

```bash
make check-deps
```

### Compile

```bash
make
```

This will create the executables `transbasket` and `cache_tool` in the project root.

### Debug Build

```bash
make debug
```

## Configuration

The server requires two configuration files:

### 설정 파일 정책 (추적 템플릿 / 로컬 실제 값)

저장소에 **추적되는 `*.conf` 는 전부 템플릿**이며 placeholder 값만 담는다. 실제 엔드포인트·API 키·
내부 호스트는 같은 디렉터리의 **미추적 `*.local.conf`** 에 둔다(`.gitignore` 가 `*.local.conf` 를 제외).

```bash
# 최초 1회: 템플릿을 복사해 실제 값을 채운다
cp transbasket.conf transbasket.local.conf
$EDITOR transbasket.local.conf        # OPENAI_BASE_URL, OPENAI_API_KEY 등

./transbasket                          # -c 없이 실행하면 transbasket.local.conf 를 자동으로 사용
```

- `-c` 나 `TRANSBASKET_CONFIG` 가 없을 때만 자동 선택이 동작한다. `transbasket.local.conf` 가 있으면
  그것을, 없으면 `transbasket.conf`(템플릿)를 읽는다. 기동 로그의
  `Using configuration file: ...` 줄로 실제 사용된 파일을 확인할 수 있다.
- codex 백엔드도 동일하다: `cp transbasket_codex.conf transbasket_codex.local.conf` 후 `-c` 로 지정.
- 환경별 배포 설정(`transbasket_dkfs_codex.conf`, `transbasket_pge4_codex.conf`)도 같은 이유로
  추적하지 않는다.

### 1. transbasket.conf

실행 파일과 **같은 디렉터리**에서 찾습니다(상대 경로는 실행 파일 위치 기준으로 해석). 위의
설정 파일 정책에 따라 `transbasket.local.conf` 가 있으면 그쪽이 먼저 사용됩니다.

```
OPENAI_BASE_URL="http://<ollama-host>:11434/v1"   # OpenAI 호환 엔드포인트
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

In codex mode set `TRANS_CACHE_THRESHOLD="1"`: codex output is non-deterministic, so a
repeated source text is re-worded on every call, which resets the cache entry's `count`
to 1 — with a higher threshold the cache never reaches it and every request re-invokes
codex. The ready-made `transbasket_codex.conf` already sets this.

codex mode requires the running user to have completed `codex login` and to have a writable `CODEX_HOME`. The endpoint translates untrusted text, so `sandbox` is forced to `read-only` and `approval-policy` to `never` unless `CODEX_ALLOW_UNSAFE=true`. See `PLAN_MODEL_TYPE_CODEX_MCP.md` for design and the operator/systemd checklist.

### 2. PROMPT_PREFIX.txt

Located at `../PROMPT_PREFIX.txt` (relative to executable):

```
translation from language to language. not add more text. note add emoji syntax. only return plain translate text.
```

### 3. SAMPLE_SOURCE_ENGLISH.txt (Test File)

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
./transbasket
```

### With Custom Configuration

```bash
./transbasket -c /path/to/config.conf -p /path/to/PROMPT_PREFIX.txt
```

### With Custom Worker Count

```bash
./transbasket -w 20
```

### Using the codex backend

Set `MODEL_TYPE="codex"` in the config (see [Configuration](#1-transbasketconf)),
make sure `codex login status` shows you are logged in, then run normally:

```bash
# Translation requests transparently route through `codex mcp-server`.
./transbasket -c /path/to/codex-config.conf

# /health reports the active backend
curl http://localhost:8889/health   # -> {"...","model_type":"codex"}
```

The HTTP request/response format is identical for both backends — only the
translation engine changes.

or

```bash
MAX_WORKERS=20 ./transbasket
```

### Command Line Options

```
Usage: transbasket [OPTIONS]

Options:
  -c, --config PATH       Path to configuration file
                          (default: transbasket.local.conf, else transbasket.conf)
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
- `from` (string, required): 원본 언어 코드 (아래 "언어 코드" 참조)
- `to` (string, required): 대상 언어 코드 (아래 "언어 코드" 참조)
- `text` (string, required): 번역할 텍스트 (**1 ~ 10,000 바이트**, 초과 시 422)

**Supported Language Codes:**

번역 지원 언어는 Model에 따라 다르며, 언어의 형식은 `language_tables.conf`를 참고하세요.

기본적으로 **ISO 639-2 3글자 코드**(`kor`, `eng`, `jpn` …) 또는 **영어 언어명**(`Korean`, `English` …)을
받습니다. 대소문자는 구분하지 않습니다.

**중국어 간체/번체 구분**

중국어는 스크립트를 구분해 요청할 수 있으며, 세 가지 표기를 모두 받습니다:

| 표기 | 예 | 내부 코드 | 번역 결과 |
|---|---|---|---|
| **BCP-47 태그** (권장) | `zh-Hans`, `zh-Hant`, `zh-Hant-TW`, `zh-Hans-SG` | `zhs` / `zht` | 간체 / 번체 |
| BCP-47 지역 태그 | `zh-CN`, `zh-SG`, `zh-MY` → 간체 / `zh-TW`, `zh-HK`, `zh-MO` → 번체 | `zhs` / `zht` | 간체 / 번체 |
| **3글자 코드** | `zhs`, `zht` | 그대로 | 간체 / 번체 |
| 언어명 | `Chinese (Simplified)`, `Chinese (Traditional)` | `zhs` / `zht` | 간체 / 번체 |
| 스크립트 미지정 | `zh`, `zho`, `chi`, `Chinese` | `chi` | 모델 기본값(대개 간체) |

- 구분자는 `-` 와 `_` 를 모두 받습니다(`zh_Hans` 가능). 대소문자 무시.
- **스크립트 서브태그가 지역보다 우선**합니다: `zh-Hant-CN`, `zh-CN-Hant` → 모두 번체.
- `zhs` / `zht` 는 ISO 639-2 코드가 아니라 **transbasket 전용 3글자 코드**입니다. 3글자 코드만 다루는
  클라이언트를 위해 공식 지원하지만, 표준 표기인 `zh-Hans` / `zh-Hant` 사용을 권장합니다.
- 캐시는 스크립트별로 분리됩니다: `zh-Hans`·`zhs`·`zh-CN` 은 같은 캐시 항목을 공유하고,
  `zh-Hant` 계열과는 별개 항목입니다. `chi` 는 또 다른 항목입니다.
- 지원하지 않는 값은 `422` 입니다: `cmn`, `yue`, `zh-yue`, `zh-latn`, `zh-xx`,
  그리고 서브태그 문법을 어긴 값(`zh-Hans-`, `zh-Hans--CN`, `zh-Hans-中文`).

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
- `translatedText` (string): 번역된 텍스트
  - `MODEL_TYPE="normal"`: 이스케이프 해제 + 이모지/shortcode 제거 + 공백 정규화가 적용되며 **8,191 바이트에서 잘립니다**(아래 한계 표 참조)
  - `MODEL_TYPE="codex"`: 앞뒤 공백 제거와 전체 코드펜스 언랩만 적용되고 **길이 제한 없음**

**Status Codes:**
- `200 OK`: 번역 성공
- `400 Bad Request`: 잘못된 JSON 형식
- `422 Unprocessable Entity`: 유효하지 않은 UUID, 언어 코드, 또는 타임스탬프
- `500 Internal Server Error`: 예상치 못한 내부 오류
- `502 Bad Gateway`: OpenAI API 4xx 오류 (재시도 불가)
- `503 Service Unavailable`: OpenAI API 5xx/타임아웃 (재시도 가능, `Retry-After: 5` 헤더 포함)
- `504 Gateway Timeout`: 요청 타임아웃

codex 백엔드의 상태 코드 매핑은 조금 다릅니다: 동시 실행 슬롯을 `CODEX_TIMEOUT` 안에 얻지 못하면 `503`,
codex 호출 자체가 `CODEX_TIMEOUT` 을 넘기면 `504`, 그 밖의 codex 실패(기동/초기화/빈 응답 등)는 `502` 입니다.

---

### 한계와 보장 (Limits and guarantees)

아래 값은 2026-08-21 기준으로 코드와 실측으로 확인된 값입니다. 상수는 `src/http_client.c`,
`src/json_handler.c`, `src/http_server.c` 에 하드코딩되어 있으며 설정으로 바꿀 수 없습니다.

#### 크기 한계

| 항목 | 값 | 초과 시 동작 |
|------|-----|--------------|
| 요청 `text` 길이 | **10,000 바이트** (`MAX_TEXT_LENGTH`) | `422 VALIDATION_ERROR` (명시적 거부) |
| 요청 `text` 최소 길이 | 1 바이트 | `422` |
| HTTP 요청 본문 전체 크기 | **상한 없음** | 서버가 메모리에 그대로 누적. 신뢰할 수 없는 클라이언트에 노출할 때는 앞단(리버스 프록시)에서 제한 필요 |
| 응답 길이 — `normal` | **8,191 바이트** (`MAX_CLEANED_TEXT_BUFFER` 8192) | ⚠️ **조용히 잘림**. HTTP 200 이고 경고 로그도 없으며, 잘린 텍스트가 캐시에도 저장됨 |
| 이스케이프 해제 중간 버퍼 — `normal` | 16,383 바이트 (`MAX_TRANSLATION_BUFFER` 16384) | 조용히 잘림 (이후 8,191 로 다시 잘림) |
| 응답 길이 — `codex` | 사실상 제한 없음 | MCP stdout 누적이 **4 MB** (`MCP_RBUF_MAX`)를 넘으면 절단이 아니라 오류(`502`) |
| 스트리밍 누적 버퍼 (`STREAM=yes`) | 65,535 바이트 (`MAX_STREAM_BUFFER`) | 조용히 잘림 (디버그 빌드에서만 로그) |

> **실측**: 고정 길이 응답을 돌려주는 테스트용 upstream 이 13,500 바이트 번역문을 반환했을 때
> `normal` 백엔드 클라이언트는 8,191 바이트만 수신했습니다(HTTP 200, 경고 없음).
> `codex` 백엔드에서는 실제 번역 결과 10,533 바이트가 절단 없이 그대로 전달되었습니다.
> `text` 가 10,000 바이트를 넘으면 두 백엔드 모두 422 로 거부됩니다.
>
> 입력이 10,000 바이트 이하여도 **영→한처럼 바이트 수가 늘어나는 방향**에서는 `normal` 백엔드의 8,191 바이트
> 상한에 걸릴 수 있습니다(한글은 UTF-8 에서 글자당 3바이트).

#### 시간 한계

| 항목 | 값 | 비고 |
|------|-----|------|
| HTTP 연결 타임아웃 | 120초 (MHD) | 설정 불가 |
| `normal` upstream 호출 타임아웃 | 60초/시도 | 최대 3회 시도, 백오프 `2^attempt` 초 (5xx·타임아웃만 재시도) |
| `codex` 호출 타임아웃 | `CODEX_TIMEOUT` (기본 100초) | 반드시 120초보다 작아야 함. 초과 시 `504` |
| `codex` 동시 실행 슬롯 대기 | `CODEX_TIMEOUT` 까지 대기 | 대기 실패 시 `503` |

#### 동시성

- 연결마다 전용 스레드가 생성됩니다(`MHD_USE_THREAD_PER_CONNECTION`).
- **`MAX_WORKERS` 는 동시 연결 수를 제한하지 않습니다.** MHD 에 `CONNECTION_LIMIT`/`THREAD_POOL_SIZE` 를
  전달하지 않기 때문이며, 이 값은 `CODEX_MAX_CONCURRENCY` 미설정 시의 codex 동시 실행 수 기본값으로만 쓰입니다.
- codex 백엔드에서 **실제로 동시에 번역되는 요청 수는 `CODEX_MAX_CONCURRENCY`** 입니다. 초과분은 즉시 거부되지 않고
  슬롯을 기다리며, `CODEX_TIMEOUT` 을 넘기면 `503` 입니다.
- **요청 간 격리는 보장됩니다.** 응답의 `uuid`/`timestamp` 는 그 요청의 값이며, 다른 요청의 번역문이 섞이지 않습니다.
  (실측: 서로 다른 12개 원문을 동시 200 스레드로 1,200건 호출 시 불일치 0건, keep-alive 연결 재사용 240건 불일치 0건,
  codex 실호출 동시 8건 8/8 일치)

#### 번역 결과의 재현성 — 보장하지 않음

- **같은 원문이라도 호출마다 표현이 달라질 수 있습니다.** 특히 `codex` 백엔드는 비결정적입니다.
- 결과는 `PROMPT_PREFIX_FILE` 내용에 좌우됩니다. 기본 제공되는 `PROMPT_PREFIX_translategemma.txt` 에는
  "전체가 ASCII 패턴이면 원문을 byte-for-byte 그대로 출력하라"는 규칙이 있어, **순수 ASCII 영문 장문이
  번역되지 않고 그대로 반환되는 경우가 실측**되었습니다(7 KB 내외 입력에서 관측; 짧은 ASCII 문장과
  비ASCII 문자가 섞인 문장은 정상 번역). 이 동작은 서버 버그가 아니라 프롬프트 규칙의 결과입니다.
- 캐시가 켜져 있고 `TRANS_CACHE_THRESHOLD=1` 이면 **첫 결과가 고정**됩니다. 첫 응답이 잘못 나오면 해당 항목이
  캐시에서 정리되기 전까지 같은 값이 계속 반환됩니다.

#### 캐시 키

- 키는 `SHA-256("<from>|<to>|<text>")` 이며, **모델·프롬프트·백엔드 종류는 키에 포함되지 않습니다.**
  서로 다른 `MODEL_TYPE`/프롬프트를 쓰는 인스턴스가 같은 캐시 DB 를 공유하면 다른 설정으로 생성된 번역문이
  반환됩니다. 기본 제공 설정은 codex 용 캐시 DB 를 분리해 두었으니 배포 시에도 분리를 유지하세요.
- 캐시 키의 `text` 는 ANSI 이스케이프·제어문자를 제거한 **정규화 후 텍스트**입니다.

## Project Structure

```
transbasket/
├── Makefile                    # Build system (binaries land in the project root)
├── README.md                   # This file
├── CLAUDE.md                   # Implementation notes for contributors
├── transbasket.conf            # Default config TEMPLATE (real values go in *.local.conf)
├── transbasket_codex.conf      # codex backend config template
├── language_tables.conf        # Language code/name table
├── PROMPT_PREFIX*.txt          # Prompt templates
├── ROLS.txt                    # System role text
├── cache_sqlite_schema.sql     # Reference schema for the sqlite cache backend
├── include/                    # Header files
│   ├── utils.h  config_loader.h  json_handler.h
│   ├── http_client.h  http_server.h  mcp_client.h
│   ├── language_table.h
│   └── trans_cache.h  cache_backend_text.h  cache_backend_sqlite.h
├── src/                        # Source files
│   ├── main.c  utils.c  config_loader.c  json_handler.c
│   ├── http_client.c  http_server.c  mcp_client.c
│   ├── language_table.c
│   ├── trans_cache.c  cache_backend_text.c  cache_backend_sqlite.c
│   └── cache_tool.c            # Cache management tool (separate binary)
├── tests/test_client.py        # Python test client
├── obj/                        # Object files (generated)
├── transbasket                 # Server binary (generated)
└── cache_tool                  # Cache tool binary (generated)
```

## Module Overview

### utils.c
- Language code validation and normalization (ISO 639-2, English names, and
  Chinese BCP-47 tags folded to `zhs`/`zht`)
- UUID v4 validation (RFC 4122)
- RFC 3339 timestamp validation
- Emoji and shortcode stripping
- Text truncation utilities

### config_loader.c
- Configuration file parsing (KEY="value" format)
- Config selection: `transbasket.local.conf` first, then `transbasket.conf`
- Prompt prefix / system role file loading
- Configuration validation (including the codex sandbox/approval hard-coercion)
- Path resolution relative to the executable

### json_handler.c
- JSON request parsing with cJSON
- JSON response generation
- Error response formatting
- Request/response validation

### http_client.c
- Translation dispatch on `MODEL_TYPE` (normal vs codex)
- normal: OpenAI API communication with libcurl, retry with exponential backoff
- codex: builds delimiter-isolated prompt, concurrency semaphore, per-request temp cwd
- Prompt building: `{TEXT}`-style templates (TranslateGemma) and the legacy
  `[TARGET LANGUAGE]` / `{{LANGUAGE_TO}}` form
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

### language_table.c
- Loads `language_tables.conf` (code→name entries and ISO 639-2↔639-1 mappings)
- Resolves plain codes and regional/script variants (`zh-Hant-HK` → Chinese (Traditional))

### trans_cache.c / cache_backend_text.c / cache_backend_sqlite.c
- Backend-agnostic cache API with a text (JSONL) and a SQLite backend
- SHA-256(`from|to|text`) cache keys via the OpenSSL EVP API
- All backend calls are serialized; lookups return a caller-owned copy
  (see "Translation Cache Concurrency" in CLAUDE.md)

### cache_tool.c
- Separate `cache_tool` binary: list / clear / stats / cleanup / search / export / migrate
- Note: every command except `migrate` operates on the **text** backend

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
