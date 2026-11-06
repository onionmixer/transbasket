# Transbasket C Implementation

Production-ready C implementation of the Transbasket translation server for optimal performance.

## Features

- High-performance HTTP server using libmicrohttpd
- Pluggable translation backend via `MODEL_TYPE`:
  - **normal**: OpenAI-compatible API client using libcurl (default)
  - **codex**: drives the installed `codex` CLI (`codex exec` per request; legacy `codex mcp-server` optional)
- JSON handling with cJSON
- Thread-per-connection model for concurrent requests
- Translation cache (text / SQLite backends), keyed per language pair
- Chinese script support: `zh-Hans` / `zh-Hant` translate and cache separately
- Automatic source language: `from: "auto"` (normal and codex backends)
- Concurrent duplicate requests share one translation and retain their own response IDs
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

The `codex` translation backend shells out to the installed Codex CLI (`codex exec`,
one process per request). It is **not** needed for the default `normal` backend.

> **codex-cli 0.154.0 removed `codex mcp-server`.** With a current CLI that subcommand
> is parsed as an interactive prompt and exits with `Error: stdin is not a terminal`, so
> the old MCP route answered every request with `502 codex MCP initialize failed`.
> `CODEX_INTERFACE="exec"` (the default) uses the supported `codex exec` surface instead;
> `CODEX_INTERFACE="mcp"` remains only for hosts pinned to an older CLI. The startup log
> prints the detected version and warns if the chosen interface is missing.

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

### Offline regression tests

```bash
make test-auto
make test-coalescing
make test-source-hint test-strict-json test-source-hint-store \
     test-cache-tool-hints test-source-hint-server test-structured-hint
```

Python 3 표준 라이브러리만 사용하며 임시 캐시와 127.0.0.1 테스트 서버를 생성합니다.
외부 모델 호출 없이 API 계약, normal·codex 프롬프트, 캐시와 SQLite 이전을 검증합니다.
2026-11-03 검증 결과는 auto 9개, 요청 병합 통합 14개 및 C 모듈 검증 통과입니다. 실제 모델의 번역 품질 검증과는 별개이며,
현재 설정의 TranslateGemma 호출은 503으로 실패했습니다.
세부 범위와 남은 검증은 [TEST_RESULTS.md](TEST_RESULTS.md)를 참고하세요.

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

#### `LISTEN` - 바인드 주소

`LISTEN` 은 서버가 실제로 바인드하는 주소입니다. 2026-11-06 수정 전에는 값과 무관하게 항상 `0.0.0.0` 에
바인드했습니다 (`127.0.0.1` 로 적어도 LAN 에서 접속됨, IPv6 불가).

| 값 | 받는 접속 |
|---|---|
| `0.0.0.0` (기본값) | 모든 IPv4 인터페이스 |
| `127.0.0.1` | 이 호스트의 IPv4 루프백만 |
| 특정 IPv4 주소 | 그 주소로 들어오는 IPv4 접속만 |
| `::` | 모든 IPv4·IPv6 (이중 스택) |
| `::1` | IPv6 루프백만 |
| 그 밖의 IPv6 주소 | 그 주소로 들어오는 IPv6 접속만 |

- **숫자 주소만** 받습니다. `localhost` 같은 호스트 이름, `[::1]`, zone(`fe80::1%eth0`), 앞뒤 공백,
  `127.1`·`010.0.0.1` 같은 축약·8진 표기는 기동 오류입니다.
- 멀티캐스트, `255.255.255.255`, IPv4-mapped(`::ffff:a.b.c.d`, IPv4 형식으로 쓰면 됨), link-local(`fe80::/10`)도 거부합니다.
- 형식은 맞지만 이 호스트에 없는 주소, 이미 쓰이는 포트는 `libmicrohttpd: ...` 원인 로그와 함께 기동 실패합니다.
- `LISTEN` 을 지정하려는 줄이 읽히지 않으면 기본값으로 넘어가지 않고 **기동을 멈춥니다**: `LISTEN = "..."`(`=` 앞뒤 공백),
  `export LISTEN=...`, 소문자 `listen=`, 511바이트를 넘는 값, 1,023바이트를 넘는 줄, NUL 바이트가 든 줄.
  `LISTEN` 줄이 여러 개면 다른 키처럼 마지막 줄이 쓰입니다.
- 다른 키의 줄에 NUL 바이트가 있으면 그 줄만 경고 후 무시합니다 (예전에는 다음 줄까지 함께 버려졌습니다).
- 기동 로그의 `HTTP server listening on 127.0.0.1:8889 (IPv4)` / `[::]:8889 (IPv4+IPv6)` 가 **실제** 바인드 주소입니다.
  배너의 `Listen (configured):` 은 설정값일 뿐입니다.
- `-d`(데몬) 모드는 설정을 읽기 전에 분리되므로 이런 기동 오류가 실행한 터미널에 보이지 않습니다. 새 값은 전경에서 먼저 확인하세요.

#### Translation backend: `MODEL_TYPE`

```
MODEL_TYPE="normal"   # normal (default) | codex
```

- **`normal`** — translate via the OpenAI-compatible HTTP API (`OPENAI_*`). Default; unchanged behavior.
- **`codex`** — translate with the installed `codex` CLI. `OPENAI_*` are ignored; the `CODEX_*` keys apply (see comments in `transbasket.conf`):

```
MODEL_TYPE="codex"
#CODEX_INTERFACE="exec"         # exec (default) | mcp (legacy, codex-cli < 0.154.0 only)
#CODEX_BIN="codex"              # codex executable (PATH-searched)
#CODEX_MODEL="gpt-5.2"          # optional model override (unset = codex default)
#CODEX_TIMEOUT="100"            # seconds (must be < MHD 120s)
#CODEX_MAX_CONCURRENCY="8"      # concurrent codex processes (default = MAX_WORKERS)
#CODEX_RUN_DIR="/var/lib/transbasket/run"   # writable root for per-request temp cwd
#CODEX_REASONING_EFFORT="none"  # optional model_reasoning_effort override (valid values depend on the model)
#CODEX_ALLOW_UNSAFE="false"     # allow non read-only/never (dev only)
#CODEX_IGNORE_USER_CONFIG="false"  # exec only: skip $CODEX_HOME/config.toml (set CODEX_MODEL too)
```

With `CODEX_INTERFACE="exec"` each request runs

```
codex exec --skip-git-repo-check --color never [--ephemeral] [--ignore-user-config] \
           -s <CODEX_SANDBOX> -c approval_policy="<CODEX_APPROVAL_POLICY>" \
           [-c model_reasoning_effort="<CODEX_REASONING_EFFORT>"] [-m <CODEX_MODEL>] \
           -C <per-request temp dir> -o <temp dir>/last-message.txt -
```

The prompt is written to **stdin** (never argv, so the untrusted text does not appear in
`ps` or `/proc/<pid>/cmdline`), and the result is read from `--output-last-message`.
`--ephemeral` is added when the installed CLI supports it, so no session files pile up
under `$CODEX_HOME/sessions`. codex echoes the whole prompt on stderr, so stderr is never
copied into client-facing errors; only its last `ERROR:` line is (e.g. an unsupported
model), and with `DEBUG=yes` the stderr tail goes into the trace file.

**MCP servers in `config.toml`.** codex starts every MCP server and plugin from the
running user's `$CODEX_HOME/config.toml` on each translation. Their tools are outside the
`read-only` sandbox, so untrusted source text could steer codex into calling them. At
startup transbasket lists them in a warning. On a host whose codex account also has MCP
servers configured, set `CODEX_IGNORE_USER_CONFIG="true"` together with an explicit
`CODEX_MODEL` (the config file's `model` is skipped too, and codex's built-in default model
may reject `CODEX_REASONING_EFFORT="none"`). Measured: with 5 configured servers one
translation spawned 9 helper processes; with the flag set it spawned none.

In codex mode set `TRANS_CACHE_THRESHOLD="1"`: codex output is non-deterministic, so a
repeated source text is re-worded on every call, which resets the cache entry's `count`
to 1 — with a higher threshold the cache never reaches it and every request re-invokes
codex. The ready-made `transbasket_codex.conf` already sets this.

codex mode requires the running user to have completed `codex login` and to have a writable `CODEX_HOME`. The endpoint translates untrusted text, so `sandbox` is forced to `read-only` and `approval_policy` to `never` unless `CODEX_ALLOW_UNSAFE=true`; `CODEX_APPROVAL_POLICY` and `CODEX_REASONING_EFFORT` must match `[A-Za-z0-9._-]{1,64}` because they are passed as TOML `-c` overrides. See `PLAN_MODEL_TYPE_CODEX_MCP.md` for design and the operator/systemd checklist.

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
# Translation requests transparently route through `codex exec`.
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
- `from` (string, required): 원본 언어 코드 또는 `auto` (아래 "언어 코드" 참조)
- `to` (string, required): 대상 언어 코드 (아래 "언어 코드" 참조)
- `text` (string, required): 번역할 텍스트 (**1 ~ 10,000 바이트**, 초과 시 422)

**Supported Language Codes:**

번역 지원 언어는 Model에 따라 다르며, 언어의 형식은 `language_tables.conf`를 참고하세요.

기본적으로 **ISO 639-2 3글자 코드**(`kor`, `eng`, `jpn` …) 또는 **영어 언어명**(`Korean`, `English` …)을
받습니다. 대소문자는 구분하지 않습니다.

**원문 언어 자동 감지**

`"from": "auto"`를 보내면 모델이 같은 번역 호출 안에서 원문 언어를 판단합니다.
`AUTO`, `Auto`도 허용하며 내부에서는 `auto`로 통일합니다. `auto`는 언어 테이블 조회와
일반 언어 검증 전에 판별하는 원문 전용 모드이므로 테이블에 등록할 필요가 없습니다.
`to`에는 구체적인 대상 언어를 지정해야 하며 `to=auto`는 422입니다.
`from` 누락·빈 값·앞뒤 공백은 자동 감지로 처리하지 않습니다.

```json
{
  "timestamp": "2026-11-03T01:23:45.678Z",
  "uuid": "550e8400-e29b-41d4-a716-446655440000",
  "from": "auto",
  "to": "kor",
  "text": "Please check the meeting schedule."
}
```

normal과 codex 모두 지원하며 응답 필드는 기존과 같습니다. 감지된 언어명·신뢰도는 반환하지 않습니다.
혼합 언어와 짧은 문장의 해석은 모델에 의존합니다.

자동 감지는 기존 `PROMPT_PREFIX_FILE` 대신 내장 자동 감지 지시를 사용합니다. 기존 템플릿의
English 고정 지시나 ASCII 원문 반환 규칙을 자동 감지 요청에 적용하지 않습니다.
필요하면 설정 파일 기준 상대 경로 또는 절대 경로로 추가 지시를 지정할 수 있습니다.

```conf
# 선택 설정. 생략하면 내장 지시만 사용합니다.
PROMPT_PREFIX_AUTO_FILE="PROMPT_PREFIX_auto.txt"
```

이 파일은 `{TARGET_LANG}`, `{TARGET_CODE}`, `[TARGET LANGUAGE]`, `{{LANGUAGE_TO}}`와
선택적인 `{TEXT}`를 지원합니다. `{TEXT}`가 있으면 normal은 단일 사용자 메시지로 전달하고,
없으면 본문을 별도 메시지로 전달합니다. codex는 두 형식 모두 요청별 구분자로 본문을 감쌉니다.
자동 감지·대상 언어·데이터 취급 지시는 서버가 앞에 붙입니다.
`{SOURCE_LANG}`·`{SOURCE_CODE}`, 빈 파일, 읽을 수 없는 파일은 시작 시 설정 오류입니다.
`SYSTEM_ROLE`은 계속 적용되므로 특정 언어를 강제하는 지시가 없는지 확인하세요.

캐시는 `auto → 대상 언어`로 저장하고 명시적 원문 언어의 캐시와 분리합니다.
SQLite의 기존 버전 0/1 DB는 시작 시 버전 2로 이전하며, 기존 행·해시·횟수·시각·인덱스·트리거를
보존합니다. 배포 전 백업 및 구버전으로의 롤백 절차는 [INSTALL.md](INSTALL.md)를 참고하세요.

오프라인 회귀 검증은 `make test-auto`로 실행합니다. 임시 캐시와 로컬 가짜 HTTP·codex 서버만 사용합니다.

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
- `422 Unprocessable Entity`: JSON 파싱 실패 또는 유효하지 않은 UUID, 언어 코드, 타임스탬프, 텍스트.
  U+0000 으로 디코드되는 `\u` 이스케이프(`\u0000`, 또는 `\u00ZZ` 처럼 뒤 4바이트가 16진수가 아닌 것)가 본문 어디에든 있는 경우도 포함
  (아래 "문자열 무결성" 참고)
- `500 Internal Server Error`: 예상치 못한 내부 오류
- `502 Bad Gateway`: OpenAI API 4xx 오류 (재시도 불가)
- `503 Service Unavailable`: OpenAI API 5xx/타임아웃 (재시도 가능, `Retry-After: 5` 헤더 포함)
- `504 Gateway Timeout`: 요청 타임아웃

codex 백엔드의 상태 코드 매핑은 조금 다릅니다: 동시 실행 슬롯을 `CODEX_TIMEOUT` 안에 얻지 못하면 `503`,
codex 호출 자체가 `CODEX_TIMEOUT` 을 넘기면 `504`, 그 밖의 codex 실패(기동/초기화/빈 응답 등)는 `502` 입니다.

---

### 동일 번역 요청 병합

동일한 원문 언어·대상 언어·전처리 후 본문이 동시에 들어오면 번역 작업 하나만 실행합니다.
먼저 `TRANS_CACHE_THRESHOLD`를 충족하는 캐시를 확인하고, 미달하거나 없으면 첫 요청이 번역하고
후속 요청은 결과를 기다립니다. 성공·실패 결과를 모두에게 전달하고 대기 목록에서 작업을 제거합니다.
각 응답은 자기 요청의 `uuid`·`timestamp`를 사용합니다(오류 응답의 timestamp는 현재 시각).

캐시 횟수는 실제 번역 작업에서 한 번만 갱신합니다. 임계값이 3일 때 같은 요청 10개를 한 번에 받아
번역 하나를 공유해도 최초 캐시 `count`는 1입니다. 이후 별도 작업에서도 같은 번역이 나오면 2, 3으로
올라가며 다음 요청부터 캐시를 사용합니다. 결과가 달라지면 기존대로 1로 초기화합니다.
완료 후에도 임계값이 미달이면 다음 요청은 새 번역 작업을 시작할 수 있습니다.

`AUTO`·`auto`, 중국어 스크립트 별칭은 기존 정규화 결과에 따라 합쳐집니다. `auto`와 `eng`는 별개이며,
UUID·timestamp는 병합 키에 포함하지 않습니다. 병합은 프로세스 하나 안에서만 동작합니다.
캐시 저장에 실패해도 해당 작업의 결과는 참여 요청에 전달합니다. 실패 결과는 캐시에 저장하지 않습니다.
엔진 재시도는 실행 작업 하나에서만 수행하므로, 재시도가 필요한 경우 upstream 시도는 여러 번일 수 있습니다.

| 설정 | 기본값 | 의미 |
|---|---|---|
| `TRANSLATION_TOTAL_TIMEOUT` | `110` | 등록부터 번역까지 공유하는 전체 예산(초), 1~110 |
| `INFLIGHT_MAX_KEYS` | `MAX_WORKERS` 값, 기본 30 | 동시에 실행하는 서로 다른 번역 키 수 |
| `INFLIGHT_MAX_WAITERS` | `256` | 전체 대기 요청 수, 실행 요청 제외 |
| `INFLIGHT_MAX_WAITERS_PER_KEY` | `64` | 한 작업의 대기 요청 수, 실행 요청 제외 |

대기 수·키 수 제한 설정은 양의 정수여야 합니다. 상한 초과 요청은 503과 `Retry-After: 5`로 반환하며,
캐시 적중·헬스체크는 번역 대기 한도에 넣지 않습니다. 키 수가 가득 차도 기존 작업의 대기 한도가 남으면 합류할 수 있습니다.
대기는 연결별 스레드를 사용하므로 연결 수 자체를 줄이지는 않습니다.

전체 예산은 재시도·백오프·codex 슬롯 대기·초기화·호출에 공통 적용하고, 후속 요청이 와도 연장하지 않습니다.
normal의 전체 시간 초과는 503, codex는 504입니다. 전체 예산 내 codex 슬롯 대기 실패는 기존대로 503입니다.
서버 종료는 대기를 깨우고 엔진을 취소한 뒤 자원을 정리합니다. 첫 연결이 끊겨도 다른 참여 요청의 번역은 계속합니다.
소켓 단절을 대기 중 즉시 감지하지 못할 수 있어 대기 자원은 완료·마감·종료 시 정리됩니다.

로그의 `Translation group ... started/joined/flushed`와 요청별 `role`, `elapsed_ms`로 공유 실행을 확인할 수 있습니다.
종료 시 `running=0, waiters=0, live=0` 로그를 확인합니다. 설계·검증 상세는
[요청 병합 계획 및 결과](PLAN_DUPLICATE_REQUEST_COALESCING.md)를 참고하세요.

### 원문 기준언어 기록 (source_hint)

`from=auto` 요청의 번역에는 원문이 실제로 무슨 언어였는지(기준언어)를 캐시에 함께 남깁니다.
**캐시 키와 HTTP 응답은 바뀌지 않습니다**(키는 `auto|to|text`, 응답은 `timestamp`·`uuid`·`translatedText`).
명시적 언어 요청은 `from` 이 곧 기준언어이므로 따로 기록하지 않습니다.

| `SOURCE_HINT_MODE` | 기준언어를 정하는 방법 |
|---|---|
| `off` | 기록하지 않음 |
| `script` (기본) | 서버가 원문의 문자 체계를 세어 판정합니다. 한글은 `kor`, 가나가 있으면 `jpn`, 한자만 있으면 `chi` 입니다. 알파벳·키릴 등 여러 언어가 쓰는 문자만 있으면 `und` 이고, **알파벳만으로 `eng` 라고 단정하지 않습니다** |
| `model` | codex 가 번역하면서 원문 언어를 함께 보고합니다(`codex exec --output-schema`). 응답은 엄격하게 검증하고, 모델 값이 무효이거나 문자 증거와 명백히 모순되면 `script` 값을 씁니다. `MODEL_TYPE=codex`·`CODEX_INTERFACE=exec` 이고 CLI 가 `--output-schema` 를 지원할 때만 동작하며, 그 밖에는 기동 경고 후 `script` 로 동작합니다 |

기록 값은 `source_hint`(3글자 코드, 내용이 없으면 `und`), `mixed`(다른 언어도 상당 부분), `origin`(`model`/`script`),
`hint_version` 입니다. 섞인 문서도 기준언어는 하나이고, 섞임은 `mixed` 로 표시합니다.

- **SQLite:** `trans_cache_source_hint` 테이블에 저장하고 `user_version` 은 2 로 둡니다. 조회는 뷰
  `trans_cache_effective_source` 를 씁니다. auto 행은 기록된 기준언어, 명시 행은 `from_lang` 이 `effective_source` 가 됩니다.
  ```sql
  SELECT from_lang, to_lang, effective_source, mixed, origin FROM trans_cache_effective_source;
  ```
- **JSONL:** auto 행에 `source_hint`, `hint_mixed`, `hint_origin`, `hint_version` 필드가 붙습니다.
- **모드 전환과 캐시:** codex 모드는 `TRANS_CACHE_THRESHOLD=1` 이라 이미 저장된 원문은 다시 번역하지 않습니다.
  그래서 `model` 로 바꿔도 기존 행의 `script` 값은 그대로이고, 새로 번역되는 원문부터 모델 값이 쌓입니다.
  기록이 없는 기존 auto 행은 `cache_tool backfill-hints` 로 `script` 값을 채울 수 있습니다(모델 값은 덮어쓰지 않음).
- **롤백:** 이 기능이 없는 바이너리도 같은 SQLite DB 를 그대로 엽니다(실측). 단 그 바이너리는 JSONL 을 저장할 때
  hint 필드를 버리고, 모델 값은 복원할 수 없습니다. 텍스트 백엔드는 롤백 전에 파일을 백업하세요.

실측(codex-cli 0.166.0, 배포 설정과 같은 조건, 사전 확정 정답): 단일 언어 24건 정확도 100%, 구조화 실패 0,
번역문 오염 0, 응답 지연 중앙값 +9.3% (`CODEX_IGNORE_USER_CONFIG=true`) / +12.7% (`false`).
설계와 검증 상세는 [PLAN_SOURCE_HINT_METADATA.md](PLAN_SOURCE_HINT_METADATA.md) 와 [TEST_RESULTS.md](TEST_RESULTS.md) 에 있습니다.

### 한계와 보장 (Limits and guarantees)

크기 제한은 2026-08-21 코드·실측 기준이며, 시간·동시성 정책은 2026-11-03 요청 병합 구현을 반영합니다.
크기 상수는 `src/http_client.c`, `src/json_handler.c`에 있고, 전체 번역 시간과 병합 한도는 위 설정으로 조정합니다.

#### 크기 한계

| 항목 | 값 | 초과 시 동작 |
|------|-----|--------------|
| 요청 `text` 길이 | **10,000 바이트** (`MAX_TEXT_LENGTH`) | `422 VALIDATION_ERROR` (명시적 거부) |
| 요청 `text` 최소 길이 | 1 바이트 | `422` |
| HTTP 요청 본문 전체 크기 | **상한 없음** | 서버가 메모리에 그대로 누적. 신뢰할 수 없는 클라이언트에 노출할 때는 앞단(리버스 프록시)에서 제한 필요 |
| 응답 길이 — `normal` | **8,191 바이트** (`MAX_CLEANED_TEXT_BUFFER` 8192) | ⚠️ **조용히 잘림**. HTTP 200 이고 경고 로그도 없으며, 잘린 텍스트가 캐시에도 저장됨 |
| 이스케이프 해제 중간 버퍼 — `normal` | 16,383 바이트 (`MAX_TRANSLATION_BUFFER` 16384) | 조용히 잘림 (이후 8,191 로 다시 잘림) |
| 응답 길이 — `codex` | 사실상 제한 없음 | 결과가 **4 MB** (`exec`: `CODEX_OUT_MAX`, `mcp`: `MCP_RBUF_MAX`)를 넘으면 절단이 아니라 오류(`502`) |
| 스트리밍 누적 버퍼 (`STREAM=yes`) | 65,535 바이트 (`MAX_STREAM_BUFFER`) | 조용히 잘림 (디버그 빌드에서만 로그) |

> **실측**: 고정 길이 응답을 돌려주는 테스트용 upstream 이 13,500 바이트 번역문을 반환했을 때
> `normal` 백엔드 클라이언트는 8,191 바이트만 수신했습니다(HTTP 200, 경고 없음).
> `codex` 백엔드에서는 실제 번역 결과 10,533 바이트가 절단 없이 그대로 전달되었습니다.
> `text` 가 10,000 바이트를 넘으면 두 백엔드 모두 422 로 거부됩니다.
>
> 입력이 10,000 바이트 이하여도 **영→한처럼 바이트 수가 늘어나는 방향**에서는 `normal` 백엔드의 8,191 바이트
> 상한에 걸릴 수 있습니다(한글은 UTF-8 에서 글자당 3바이트).

#### 문자열 무결성 (cJSON 의 NUL 잘림)

cJSON 1.7.15 는 `\u0000` 과, 뒤 4바이트가 16진수가 아닌 모든 `\u` 이스케이프(`\u00ZZ`, `\u-123` 등)를 U+0000 으로
디코드하고, C 문자열은 거기서 끊깁니다. 2026-11-06 이전에는 이 때문에 **잘린 요청이 번역되고, 잘린 번역문이 200 으로 응답·캐시**되었습니다.
지금은 cJSON 이 실제로 읽은 바이트를 `json_decodes_nul()` 로 검사해 거부합니다 ([PLAN_CLEANUP_DAYS_AND_NUL.md](PLAN_CLEANUP_DAYS_AND_NUL.md)).

| 입력 | 동작 |
|------|------|
| 요청 본문 (값·키·`uuid`·언어 필드 모두) | `422 VALIDATION_ERROR`, upstream·codex 호출 없음 |
| `normal` 응답 (비스트리밍 본문, 각 SSE `data:` 페이로드) | `502`, 재시도·캐시 없음. 한 청크라도 걸리면 응답 전체가 실패합니다 |
| codex MCP `tools/call` 응답 | `502`, 캐시 없음 |
| codex exec 출력(plain)에 원시 NUL 바이트 | `502`, 캐시 없음 |
| codex structured 출력 | 기존대로 `strict_json_validate()` 가 거부 (`502`) |

영향이 없는 것: 백슬래시를 이스케이프한 문자 그대로의 `\\u0000`, `\u00e9`·서로게이트 짝 같은 정상 이스케이프,
지금도 무시되는 SSE 주석·깨진 청크·루트 값 뒤의 잔여 바이트.

#### 시간 한계

| 항목 | 값 | 비고 |
|------|-----|------|
| HTTP 연결 타임아웃 | 비활성 120초 (MHD) | 애플리케이션의 전체 작업 마감과 별개 |
| 전체 번역 예산 | `TRANSLATION_TOTAL_TIMEOUT`, 기본 110초 | 슬롯 대기·초기화·재시도·백오프·호출에서 공유 |
| `normal` upstream 호출 타임아웃 | 최대 60초/시도와 남은 전체 예산 중 작은 값 | 최대 3회 시도, 백오프 `2^attempt` 초, 전체 마감 후 추가 시도 없음 |
| `codex` 호출 타임아웃 | `CODEX_TIMEOUT` (기본 100초) | 반드시 120초보다 작아야 함. 초과 시 `504` |
| `codex` 동시 실행 슬롯 대기 | `CODEX_TIMEOUT`과 남은 전체 예산 이내 | 슬롯 대기 실패 `503`, 전체 작업 마감 `504` |
| 캐시 정리 기간 | `TRANS_CACHE_CLEANUP_DAYS`, 기본 60일 | 64비트로 계산 (예전에는 24,856일 이상에서 전체 삭제 가능). `atoi` 와 같은 값으로 읽고, int 범위를 넘으면 최댓값으로 포화(사실상 만료 없음)하며 경고. 확인 주기는 기간의 1/10, 최소 1시간. 기동 로그 `Cache background thread started (... cleanup after N days)` 에 실효값 |

#### 동시성

- 연결마다 전용 스레드가 생성됩니다(`MHD_USE_THREAD_PER_CONNECTION`).
- **`MAX_WORKERS` 는 동시 연결 수를 제한하지 않습니다.** MHD 에 `CONNECTION_LIMIT`/`THREAD_POOL_SIZE` 를
  전달하지 않기 때문입니다. 이 값은 `INFLIGHT_MAX_KEYS`와 `CODEX_MAX_CONCURRENCY` 미설정 시의 기본값입니다.
- 서로 다른 활성 번역 키는 `INFLIGHT_MAX_KEYS`로 제한하며, 같은 키의 요청은 한 작업을 공유합니다.
- codex 백엔드에서 **실제로 동시에 번역되는 요청 수는 `CODEX_MAX_CONCURRENCY`** 입니다. 초과분은 즉시 거부되지 않고
  슬롯을 기다립니다. 병합 대기 한도와 전체 작업 마감도 함께 적용됩니다.
- **요청 간 격리는 보장됩니다.** 응답의 `uuid`/`timestamp` 는 그 요청의 값이며, 다른 요청의 번역문이 섞이지 않습니다.
  (실측: 서로 다른 12개 원문을 동시 200 스레드로 1,200건 호출 시 불일치 0건, keep-alive 연결 재사용 240건 불일치 0건,
  codex 실호출 동시 8건 8/8 일치)

#### 번역 결과의 재현성 — 보장하지 않음

- **같은 원문이라도 호출마다 표현이 달라질 수 있습니다.** 특히 `codex` 백엔드는 비결정적입니다.
- 명시적 원문 언어 요청의 결과는 `PROMPT_PREFIX_FILE` 내용에 좌우됩니다. 기본 제공되는 `PROMPT_PREFIX_translategemma.txt` 에는
  "전체가 ASCII 패턴이면 원문을 byte-for-byte 그대로 출력하라"는 규칙이 있어, **순수 ASCII 영문 장문이
  번역되지 않고 그대로 반환되는 경우가 실측**되었습니다(7 KB 내외 입력에서 관측; 짧은 ASCII 문장과
  비ASCII 문자가 섞인 문장은 정상 번역). 이 동작은 서버 버그가 아니라 프롬프트 규칙의 결과입니다.
- `from=auto`는 별도의 내장 지시와 선택적인 `PROMPT_PREFIX_AUTO_FILE`을 사용하므로 위 템플릿의
  ASCII 원문 반환 규칙을 적용하지 않습니다. 모델 출력과 엔진별 후처리까지 원문 보존을 보장하는 것은 아닙니다.
- 캐시가 켜져 있고 `TRANS_CACHE_THRESHOLD=1` 이면 **첫 결과가 고정**됩니다. 첫 응답이 잘못 나오면 해당 항목이
  캐시에서 정리되기 전까지 같은 값이 계속 반환됩니다.

#### 캐시 키

- 키는 `SHA-256("<from>|<to>|<text>")` 이며, **모델·프롬프트·백엔드 종류는 키에 포함되지 않습니다.**
  서로 다른 `MODEL_TYPE`/프롬프트를 쓰는 인스턴스가 같은 캐시 DB 를 공유하면 다른 설정으로 생성된 번역문이
  반환됩니다. 기본 제공 설정은 codex 용 캐시 DB 를 분리해 두었으니 배포 시에도 분리를 유지하세요.
- 캐시 키의 `text` 는 ANSI 이스케이프·제어문자를 제거한 **정규화 후 텍스트**입니다.
- `from=auto`는 소문자 `auto`를 키에 사용하며, `auto|kor|text`와 `eng|kor|text`는 별도 항목입니다.
  자동 감지 결과를 추측하여 명시적 원문 언어 캐시를 재사용하지 않습니다.

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
├── cache_sqlite_schema.sql     # Version 2 schema for NEW sqlite caches
├── PLAN_SOURCE_LANGUAGE_AUTO.md # auto design, implementation and validation status
├── include/                    # Header files
│   ├── utils.h  config_loader.h  json_handler.h
│   ├── http_client.h  http_server.h  codex_exec.h  mcp_client.h
│   ├── inflight_translation.h  translation_control.h
│   ├── language_table.h
│   └── trans_cache.h  cache_backend_text.h  cache_backend_sqlite.h
├── src/                        # Source files
│   ├── main.c  utils.c  config_loader.c  json_handler.c
│   ├── http_client.c  http_server.c  codex_exec.c  mcp_client.c
│   ├── inflight_translation.c   # Shared running translations and waiter lifetime
│   ├── language_table.c
│   ├── trans_cache.c  cache_backend_text.c  cache_backend_sqlite.c
│   └── cache_tool.c            # Cache management tool (separate binary)
├── tests/
│   ├── test_client.py          # Manual client (uses the configured real server)
│   ├── test_source_auto.py     # Offline integration tests (make test-auto)
│   ├── fake_codex_auto.py      # exec/MCP fixture; no external model calls
│   ├── test_request_coalescing.py # Concurrent HTTP/codex integration tests
│   └── test_inflight_translation.c # Lifecycle, limits, failure and race checks
├── obj/                        # Object files (generated)
├── transbasket                 # Server binary (generated)
└── cache_tool                  # Cache tool binary (generated)
```

## Module Overview

### utils.c
- Language code validation and normalization (ISO 639-2, English names, and
  Chinese BCP-47 tags folded to `zhs`/`zht`)
- Source-only `auto` detection before general language normalization
- UUID v4 validation (RFC 4122)
- RFC 3339 timestamp validation
- Emoji and shortcode stripping
- Text truncation utilities

### config_loader.c
- Configuration file parsing (KEY="value" format)
- Config selection: `transbasket.local.conf` first, then `transbasket.conf`
- Prompt prefix / system role file loading
- Optional `PROMPT_PREFIX_AUTO_FILE` loading and source-placeholder rejection
- Configuration validation (including the codex sandbox/approval hard-coercion)
- Path resolution: the config file relative to the executable, prompt/role/language
  files relative to the config file's directory

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
- `auto` branch bypasses source language lookup and resolves only the target
- Shared monotonic deadline/cancellation across retries, codex slot waits and I/O
- Error handling and status code mapping

### codex_exec.c
- Default codex interface: one `codex exec` child per request, prompt on stdin
- Startup probe: `codex --version`, `codex exec --help` (flag detection), `codex mcp list --json`
- Spawns codex in its own process group, kills the whole group on timeout
- Collects the final message from `--output-last-message` (stdout fallback), bounded at 4 MB

### mcp_client.c
- Legacy (`CODEX_INTERFACE="mcp"`): MCP stdio JSON-RPC 2.0 client for `codex mcp-server`,
  which only exists in codex-cli < 0.154.0
- Spawns codex in its own process group (`posix_spawnp`), reaps the whole tree
- initialize handshake + `tools/call` (name="codex"), id-matched responses
- Drains stdout/stderr concurrently (deadlock-safe), EINTR-tolerant, bounded buffers

### http_server.c
- HTTP server with libmicrohttpd
- Thread-per-connection model
- Health check endpoint
- Translation endpoint
- Error response handling

### inflight_translation.c
- One owner per normalized language pair and cleaned source text, with cache recheck
- Immutable result/error shared by all waiters, independent request UUID/timestamp responses
- Bounded admission, monotonic waiting, shutdown wakeup and reference-counted cleanup

### language_table.c
- Loads `language_tables.conf` (code→name entries and ISO 639-2↔639-1 mappings)
- Resolves plain codes and regional/script variants (`zh-Hant-HK` → Chinese (Traditional))

### trans_cache.c / cache_backend_text.c / cache_backend_sqlite.c
- Backend-agnostic cache API with a text (JSONL) and a SQLite backend
- SHA-256(`from|to|text`) cache keys via the OpenSSL EVP API
- All backend calls are serialized; lookups return a caller-owned copy
  (see "Translation Cache Concurrency" in CLAUDE.md)
- Source buffers accept `auto`; SQLite migrates known version 0/1 caches to version 2 atomically

### source_hint.c / strict_json.c
- `source_hint.c`: script classifier for the source_hint metadata (strict UTF-8, a generated
  Unicode 13.0.0 range table in `source_hint_table.h`, integer thresholds). Specification and
  differential test oracle: `tests/source_hint_ref.py`
- `strict_json.c`: strict validator for codex structured output, run before cJSON. Specification:
  `tests/strict_json_ref.py`

### cache_tool.c
- Separate `cache_tool` binary: list / clear / stats / cleanup / search / export / migrate / backfill-hints
- Note: every command except `migrate` and `backfill-hints` operates on the **text** backend
- `cleanup <days>` and `delete <id>` accept only a decimal integer in 1..2147483647; anything else
  (`30days`, `+30`, `4294967297`, ...) exits 1 and deletes nothing (they used to wrap via `atoi`)
- `list`/`stats` show the source_hint of auto rows; `migrate` carries hints (an existing destination
  row keeps its translation, hints merge by priority: model > script)
- `backfill-hints --backend <text|sqlite> --config <path> [--dry-run]` fills missing/outdated script hints
- Language filters accept `auto` only as source, including case-insensitive `AUTO`

text 캐시의 자동 감지 항목은 다음처럼 관리합니다. `-f` 등 전역 옵션은 명령 앞에 둡니다.

```bash
./cache_tool -f /actual/dictionary.jsonl list auto kor
./cache_tool -f /actual/dictionary.jsonl search AUTO kor "Hello"
./cache_tool -f /actual/dictionary.jsonl export auto kor
./cache_tool -f /actual/dictionary.jsonl clear auto kor
```

`clear`는 지정 언어쌍의 항목을 삭제합니다. SQLite와 text 간 이전은 `cache_tool migrate`를 사용하며,
기존 SQLite DB를 열면 스키마 이전도 실행될 수 있으므로 먼저 [백업 절차](INSTALL.md)를 따르세요.

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
