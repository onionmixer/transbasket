# Transbasket Installation Guide

> 이 문서는 **C 구현** 기준입니다. 초기 Python POC(`poc/`)는 폐기되었고 저장소에서 제거되었습니다.
> 설정 키 전체 목록과 API 스펙은 `README.md`, codex 백엔드의 처음 설계는 `PLAN_MODEL_TYPE_CODEX_MCP.md` (그 MCP 경로는 제거됨) 를 참고하세요.

## 1. 사전 요구사항

- GCC (C11) + make
- 라이브러리: libcurl, libmicrohttpd, cJSON, libuuid, OpenSSL, SQLite3
- `MODEL_TYPE=codex` 로 쓸 경우: 설치된 `codex` CLI + 해당 사용자 계정의 `codex login`

```bash
# Ubuntu / Debian
sudo apt-get install -y build-essential pkg-config \
    libcurl4-openssl-dev libmicrohttpd-dev libcjson-dev \
    uuid-dev libssl-dev libsqlite3-dev

# RHEL / CentOS / Fedora
sudo yum install -y gcc make pkg-config \
    libcurl-devel libmicrohttpd-devel cjson-devel \
    libuuid-devel openssl-devel sqlite-devel
```

## 2. 빌드

```bash
make check-deps     # 라이브러리 설치 여부 점검
make                # 릴리스 빌드
make debug          # 디버그 빌드 (-g3 -O0 -DDEBUG, core dump)
make clean          # 산출물 삭제
```

빌드 산출물은 **프로젝트 루트**에 생성됩니다:

- `transbasket` — 번역 서버 데몬
- `cache_tool` — 번역 캐시 관리 도구

`sudo make install` 은 `transbasket` 만 `/usr/local/bin/` 에 설치합니다.

## 3. 설정

설정 파일은 shell 스타일 `KEY="value"` 포맷입니다.

> **정책**: 저장소에 추적되는 `*.conf` 는 전부 **템플릿**(placeholder 전용)입니다. 실제 엔드포인트·
> API 키·내부 호스트는 같은 디렉터리의 **미추적 `*.local.conf`** 에 두세요(`.gitignore` 대상).
> `-c` 나 `TRANSBASKET_CONFIG` 없이 실행하면 `transbasket.local.conf` 가 있으면 그것을, 없으면
> `transbasket.conf` 를 읽습니다. 기동 로그의 `Using configuration file: ...` 로 확인할 수 있습니다.
>
> ```bash
> cp transbasket.conf transbasket.local.conf   # 최초 1회
> $EDITOR transbasket.local.conf               # 실제 값 입력
> ```

용도별 템플릿:

| 파일 | 용도 |
|------|------|
| `transbasket.conf` | 기본 설정 (`MODEL_TYPE="normal"`, OpenAI 호환 HTTP API) |
| `transbasket_codex.conf` | codex 백엔드용 (`MODEL_TYPE="codex"`) |
| `transbasket_example_cache_file.conf` / `transbasket_example_cache_sqlite.conf` | 캐시 백엔드별 예시 |

프롬프트/언어 테이블 파일도 함께 필요합니다: `PROMPT_PREFIX.txt`(또는 `PROMPT_PREFIX_translategemma.txt`), `ROLS.txt`, `language_tables.conf`.
`auto` 요청은 내장 지시를 사용하므로 별도 파일이 필요하지 않습니다. 선택 설정
`PROMPT_PREFIX_AUTO_FILE`을 사용하면 해당 파일도 배포해야 하며, 파일 오류는 서버 시작을 중단합니다.

상대 경로의 기준은 키마다 다릅니다 (2026-11-06 실측): 설정 파일 자체와 `LANGUAGE_TABLE_FILE` 은 **실행 파일 디렉터리**,
`PROMPT_PREFIX_FILE`·`PROMPT_PREFIX_AUTO_FILE`·`-p`·`-r` 은 **설정 파일 디렉터리**, `TRANS_CACHE_FILE`·`TRANS_CACHE_SQLITE_PATH`·
`CODEX_RUN_DIR`·`./trace/` 는 **작업 디렉터리**. 다른 위치에서 실행할 때는 절대 경로를 권장합니다.

## 4. 실행

```bash
./transbasket                                   # transbasket.local.conf → 없으면 transbasket.conf
./transbasket -c /etc/transbasket/transbasket.conf
./transbasket -c transbasket_codex.conf         # codex 백엔드
./transbasket -w 20                             # 워커 수 (codex 동시성·병합 키 한도의 기본값)
MAX_WORKERS=20 ./transbasket                    # 환경변수로도 지정 가능
./transbasket -d -l transbasket.log -c /etc/transbasket.conf   # 데몬 모드, 로그는 파일로
```

옵션: `-c/--config`, `-p/--prompt`, `-r/--role`, `-w/--workers`, `-d/--daemon`, `-l/--log-file` (`-d` 와 함께), `-h/--help`.

`-d` 는 서버가 실제로 바인드할 때까지 기다립니다. 그동안의 기동 로그가 실행한 터미널(stderr)에 보이고, 성공하면
`transbasket is running in the background (pid N)` 과 함께 종료 코드 0, 실패하면 원인 로그와 함께 1 입니다. 60초 안에 결과가 없으면
("may still be starting") 1 로 끝나지만 데몬은 계속 뜰 수 있습니다. 작업 디렉터리와 umask 는 실행한 쪽 그대로라 상대 경로가 전경과
같은 곳을 가리킵니다. `-l` 이 없으면 기동 뒤의 로그는 버려집니다.
환경변수: `TRANSBASKET_CONFIG`, `MAX_WORKERS`. 워커 수는 스레드 풀·연결 수 한도가 아니라 (연결마다 스레드),
`CODEX_MAX_CONCURRENCY`·`INFLIGHT_MAX_KEYS` 를 따로 정하지 않았을 때의 기본값입니다. 권장값은 `2 * CPU_CORES + 1` 입니다.

## 5. 동작 확인

```bash
# 헬스체크 (model_type 으로 활성 백엔드 확인)
curl http://localhost:8889/health
# -> {"status":"healthy","service":"transbasket","version":"1.0.0","model_type":"codex"}

# 번역 요청
curl -X POST http://localhost:8889/translate \
  -H "Content-Type: application/json" \
  -d '{
    "timestamp": "2026-08-21T00:00:00.000Z",
    "uuid": "550e8400-e29b-41d4-a716-446655440000",
    "from": "kor",
    "to": "eng",
    "text": "안녕하세요"
  }'
```

성공 응답은 요청의 `uuid` / `timestamp` 를 그대로 돌려줍니다. 오류 응답의 `timestamp` 는 응답 시각이고, 422 검증 오류에는 `uuid` 가 없습니다.

자동 감지는 같은 엔드포인트에서 `from`만 `auto`로 지정합니다.

```bash
curl -X POST http://localhost:8889/translate \
  -H "Content-Type: application/json" \
  -d '{
    "timestamp": "2026-11-03T01:23:45.678Z",
    "uuid": "550e8400-e29b-41d4-a716-446655440000",
    "from": "auto",
    "to": "kor",
    "text": "Please check the meeting schedule."
  }'
```

`auto`는 언어 테이블에 추가하지 않습니다. `to=auto`는 422이며,
응답에 감지된 언어명이나 신뢰도는 추가되지 않습니다.
2026-11-03의 오프라인 검증은 9개 통과했고 실제 TranslateGemma 호출은 503으로 실패했습니다 (normal 실측은 아직 없음).
실제 codex 로는 2026-11-06 에 32건을 돌려 원문 기준언어 24/24, 구조화 출력 실패 0, 번역문 오염 0 이었습니다 (번역 품질 자체는 채점하지 않음).
배포 후 실제 대상 모델에서 자동 감지·번역 결과를 확인하세요. 세부 검증 기록은 [TEST_RESULTS.md](TEST_RESULTS.md)에 있습니다.

## 6. codex 백엔드 추가 준비

```bash
codex --version          # CLI 설치 확인 (0.166.0, 0.167.0 에서 검증)
codex login status       # 서버를 '실행하는 계정'이 로그인되어 있어야 함
codex exec --help        # 사용하는 인터페이스(exec)가 있는지
codex mcp list           # 이 계정의 MCP 서버 - 있으면 아래 CODEX_IGNORE_USER_CONFIG 참고
```

체크 포인트:

- 인터페이스는 `codex exec` 하나뿐입니다. 레거시 `CODEX_INTERFACE="mcp"` (`codex mcp-server`, codex-cli 0.154.0 에서 제거됨)
  는 2026-11-06 에 지웠고, codex 모드에서 그 값이면 기동하지 않습니다 (§14). 기동 로그의
  `codex CLI: codex-cli X.Y.Z (interface=exec)` 줄과 그 아래 `Warning:` 으로 CLI 상태를 확인할 수 있습니다.
- 서비스 계정의 `config.toml` 에 MCP 서버/플러그인이 있으면 매 번역마다 함께 기동되고, 그 도구는
  read-only 샌드박스 밖입니다. 번역 전용 계정을 쓰거나 `CODEX_IGNORE_USER_CONFIG="true"` +
  `CODEX_MODEL` 을 지정하세요.

- 서비스 계정이 쓰기 가능한 `CODEX_HOME`(기본 `~/.codex`)을 가져야 codex 호출이 성공합니다.
- `CODEX_RUN_DIR`(요청별 임시 cwd 루트, 기본 `/var/lib/transbasket/run`)은 없으면 기동 시 0700 으로 만들어 봅니다
  (`Created CODEX_RUN_DIR: ...`). 서비스 계정이 만들 수 없는 위치면 경고 후 요청마다 `mkdtemp` 가 실패해, codex 가 임시 디렉터리 없이
  서버의 작업 디렉터리에서 실행되고 structured 모드 (`SOURCE_HINT_MODE=model`) 도 쓰지 못합니다. 운영에서는 미리 만들어 두는 편이 안전합니다.
- **`TRANS_CACHE_THRESHOLD="1"` 권장** (로더 기본값은 5) — codex 출력은 비결정적이라 임계값이 2 이상이면
  캐시 count 가 대개 1 로 되돌아가 캐시가 사실상 쓰이지 않고 매 요청마다 codex 를 호출합니다 (실측: 임계값 3, 같은 요청 4번 → codex 4번).
- `CODEX_MAX_CONCURRENCY` 로 동시 codex 프로세스 수를 제한하세요(요청당 프로세스 1개 spawn).

운영/systemd 상세 점검표는 `PLAN_MODEL_TYPE_CODEX_MCP.md` §7.1 을 참고하세요.

## 7. systemd 서비스

`/etc/systemd/system/transbasket.service`:

```ini
[Unit]
Description=Transbasket Translation Service
After=network.target

[Service]
Type=simple
User=transbasket
Group=transbasket
WorkingDirectory=/opt/transbasket
ExecStart=/opt/transbasket/transbasket -c /etc/transbasket/transbasket.conf -p /etc/transbasket/PROMPT_PREFIX.txt
Restart=always
RestartSec=5

# codex 백엔드를 쓸 때만 필요 (서비스 계정이 쓸 수 있는 경로)
# Environment="CODEX_HOME=/var/lib/transbasket/codex"

StandardOutput=journal
StandardError=journal
SyslogIdentifier=transbasket

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now transbasket
sudo journalctl -u transbasket -f
```

## 8. auto 지원 버전으로 업그레이드

`from=auto`는 설정 추가 없이 활성화됩니다. 기존 명시적 언어 요청은 기존 프롬프트를 사용하고,
auto는 내장 자동 감지 지시를 사용합니다. 선택 설정 `PROMPT_PREFIX_AUTO_FILE`은 설정 파일
기준 상대 경로로 해석되며, 예제는 `PROMPT_PREFIX_auto.txt`입니다.

1. 기존 서버와 같은 캐시를 사용하는 프로세스를 정지합니다.
2. 실제 설정의 SQLite 캐시를 SQLite 백업 기능으로 백업합니다. WAL 사용 중에는 DB 파일만
   복사하지 않습니다. 예: `sqlite3 /actual/cache.db '.backup /backup/cache-before-auto.db'`.
   text 캐시를 쓰면 JSONL 파일도 백업합니다.
3. `make`로 서버와 `cache_tool`을 함께 빌드하고 배포합니다. SQLite 0/1 스키마는 초기화 시
   트랜잭션 안에서 버전 2로 이전됩니다. 기존 행과 해시, 추가 제약, 인덱스·트리거·뷰 및 ID 순번을 보존합니다.
   `cache_sqlite_schema.sql`은 신규 DB용이며 기존 DB에 재실행하여 이전하는 용도로 사용하지 않습니다.
4. 시작 로그에서 캐시 초기화 성공을 확인하고 `from=auto` 요청과 반복 요청을 확인합니다.
   이전 실패는 롤백됩니다. 기존 서버 정책에 따라 캐시 초기화 실패 시 캐시 없이 실행될 수 있으므로,
   `Cannot initialize/migrate translation cache schema` 또는 `continuing without cache` 로그가 있으면
   캐시가 정상 이전된 것으로 간주하지 말고 원인을 해결합니다.

구버전으로 롤백할 때는 서버를 정지하고 배포 전 캐시 백업을 복원하거나 별도 캐시 경로를 지정합니다.
구버전은 `auto`를 `aut`로 잘라 읽을 수 있어 새 캐시를 그대로 공유하면 안 됩니다.
미래 버전·알 수 없는 구조의 DB는 자동 덮어쓰지 않습니다.

오프라인 검증: `make test-auto`. 외부 모델 호출 없이 임시 DB와 127.0.0.1 서버로 동작합니다.

## 9. 동일 요청 병합 설정

같은 번역 키의 동시 요청은 서버 프로세스 안에서 결과를 공유합니다. `TRANS_CACHE_THRESHOLD`는
계속 적용하며, 대기 요청 수만큼 캐시 횟수를 올리지 않습니다. 스키마는 기존 버전 2 그대로입니다.

```conf
TRANSLATION_TOTAL_TIMEOUT="110"
# INFLIGHT_MAX_KEYS: 생략하면 MAX_WORKERS 사용
#INFLIGHT_MAX_KEYS="30"
INFLIGHT_MAX_WAITERS="256"
INFLIGHT_MAX_WAITERS_PER_KEY="64"
```

전체 예산은 1~110초이며 슬롯 대기·재시도·백오프·호출에 함께 적용합니다.
기존 normal의 60초×3회 호출이 110초 이상 지속되던 경우 이제 전체 마감으로 503이 될 수 있습니다.
codex의 전체 마감은 504입니다. 대기 한도 초과는 503·Retry-After로 반환합니다.
`MAX_WORKERS`는 연결 수 제한이 아니지만, 이제 미설정 `INFLIGHT_MAX_KEYS`의 기본값도 결정합니다.
프록시·클라이언트 타임아웃은 번역 예산과 종료 정리 시간을 고려해 설정하세요.

기동 로그의 `Translation groups initialized`에서 실제 한도를 확인합니다.
요청별 그룹 ID·owner/waiter·elapsed_ms와 `flushed` 로그로 공유 실행을 추적할 수 있습니다.
종료 후 `Translation groups drained (running=0, waiters=0, live=0, ...)`를 확인합니다.
SIGTERM/SIGINT는 실행 취소와 대기 해제 후 종료하며, SIGHUP 캐시 저장은 메인 루프에서 처리합니다.

오프라인 검증은 `make test-coalescing`과 `make test-auto`입니다. 실제 모델 품질을 검사하는 명령은 아닙니다.
새 normal 경로는 빈 번역문이나 content가 없는 응답을 502로 처리하므로 잘못된 결과가 캐시에 저장되지 않습니다.

## 10. 원문 기준언어(source_hint) 기능으로 업그레이드

1. **백업:** SQLite 캐시와 JSONL 캐시를 복사해 둡니다.
2. 새 바이너리로 기동하면 SQLite 캐시에 `trans_cache_source_hint`·`trans_cache_hint_code` 테이블, 삭제 트리거, 뷰
   `trans_cache_effective_source` 가 만들어집니다. `user_version` 은 2 그대로이고 기존 행은 바뀌지 않습니다.
3. `SOURCE_HINT_MODE` 를 정합니다 (기본 `script`). `model` 은 `MODEL_TYPE="codex"`·`CODEX_INTERFACE="exec"` 와
   `--output-schema` 를 지원하는 codex 가 필요합니다. 기동 로그에 `Warning: SOURCE_HINT_MODE="model" ...` 이 보이면
   조건이 맞지 않아 `script` 로 동작 중인 것입니다.
4. (선택) 기존 auto 행에 기준언어 채우기:
   ```bash
   ./cache_tool backfill-hints --backend sqlite --config <캐시 DB> --dry-run   # 건수만 확인
   ./cache_tool backfill-hints --backend sqlite --config <캐시 DB>
   ```
5. 확인:
   ```bash
   sqlite3 <캐시 DB> "SELECT origin, effective_source, count(*) FROM trans_cache_effective_source WHERE from_lang='auto' GROUP BY 1,2;"
   ```

**롤백:** 이전 바이너리는 같은 SQLite DB 를 그대로 엽니다. 롤백 중 새로 쌓인 auto 행에는 기준언어가 없으므로,
다시 올린 뒤 `backfill-hints` 로 채울 수 있습니다. **텍스트(JSONL) 캐시는 이전 바이너리가 저장할 때 hint 필드를 모두 버리고,
모델 값은 복원할 수 없으므로** 롤백 전에 백업을 따로 보관합니다.

**설정 줄 길이:** 값이 511바이트를 넘거나 한 줄이 1,023바이트를 넘으면 그 줄은 경고 후 무시됩니다
(예전에는 스택 버퍼를 넘어 기동 시 abort 했습니다). `LISTEN` 줄이면 무시하지 않고 기동 오류입니다 (12절).

## 11. cleanup 기간·NUL 검사 수정으로 업그레이드

스키마·캐시 형식은 바뀌지 않습니다. 바뀌는 동작:

1. U+0000 으로 디코드되는 `\u` 이스케이프(`\u0000`, `\u00ZZ` 등)가 든 **요청은 422** 입니다 (예전: 앞부분만 번역해 200).
   upstream·codex 응답에 있으면 **502** 이고 캐시에 남지 않습니다 (예전: 잘린 번역문을 200 으로 응답·캐시).
2. `TRANS_CACHE_CLEANUP_DAYS` 는 64비트로 계산합니다. int 범위를 넘는 값은 최댓값으로 포화하고 경고합니다.
   숫자 뒤에 글자가 붙은 값(`120days`)은 예전처럼 120 으로 읽되 경고합니다. 확인:
   ```bash
   grep "Cache background thread started" <로그>      # ... cleanup after N days
   grep "Warning: TRANS_CACHE_CLEANUP_DAYS" <로그>     # 없어야 정상
   ```
3. `cache_tool cleanup <days>` / `delete <id>` 는 10진 정수 1..2147483647 만 받습니다. 스크립트에서 `30days` 같은
   값을 넘기고 있었다면 고쳐야 합니다 (이제 종료 코드 1, 아무것도 지우지 않음).

**롤백:** 이전 바이너리로 그대로 돌아갈 수 있습니다 (DB·JSONL 형식 동일). 다만 위 결함들도 함께 돌아옵니다.

## 12. `LISTEN` 적용 수정으로 업그레이드

이전 바이너리는 `LISTEN` 값을 무시하고 항상 `0.0.0.0` 에 바인드했습니다. 새 바이너리는 값대로 바인드하고,
잘못된 값이나 읽히지 않는 `LISTEN` 줄이 있으면 **기동하지 않습니다** (종료 코드 1). 스키마·캐시 형식은 그대로입니다.

1. 배포 설정의 `LISTEN` 을 확인합니다. `0.0.0.0` 이면 동작이 같습니다.
   ```bash
   grep -n -i "listen" <설정 파일>      # LISTEN="숫자 주소" 한 줄이어야 함
   ```
   `localhost` 같은 호스트 이름은 `127.0.0.1`(또는 `::1`)로 바꿉니다. `LISTEN="127.0.0.1"` 을 넣어 두었던 설정은 이제
   정말로 로컬 전용이 되므로, 다른 호스트에서 접속하던 클라이언트가 있다면 끊깁니다.
2. 새 바이너리를 **전경에서** 한 번 띄워 기동 로그를 봅니다 (§13 이전 바이너리의 `-d` 모드에서는 기동 오류가 보이지 않습니다).
   ```bash
   grep "HTTP server listening on" <로그>    # 예: 0.0.0.0:8889 (IPv4)
   ss -ltn | grep 8889
   ```
3. IPv6 로도 받으려면 `LISTEN="::"`(IPv4+IPv6) 를 씁니다. 방화벽 규칙이 IPv6 에도 있는지 확인하세요.

**롤백:** 이전 바이너리로 돌아가면 다시 `LISTEN` 을 무시하고 모든 IPv4 인터페이스에서 받습니다.

## 13. 데몬 모드·로그·normal 출력·설정 수정으로 업그레이드

스키마·캐시 형식은 그대로입니다. 바뀌는 동작 ([PLAN_OPEN_ISSUES.md](PLAN_OPEN_ISSUES.md)):

1. **설정 문법**: 따옴표 값 뒤에 글자가 있으면 (`KEY="1" # 주석`, `KEY="a`) 기동 오류입니다. 새 바이너리를 전경에서 한 번 띄우면
   `Error: line N: the value of KEY starts with a quote but does not end with the same quote` 로 줄 번호를 알려 줍니다 (주석은 자기 줄로).
   추적 템플릿 `transbasket.conf`·`transbasket_codex.conf` 는 이 문제로 기동하지 못했었고, 이제 그대로 기동합니다.
2. **숫자 설정**: 범위를 넘는 값은 감기지 않고 포화합니다 (`PORT=4294975185` 가 7889 가 되던 것 → 범위 오류). `nan`/`inf` 는 기동 오류.
   `Warning: <KEY> '...' is not a plain integer` 가 보이면 값을 고칩니다.
3. **`-d`**: 이제 기동 결과를 종료 코드로 알려 주고, 작업 디렉터리를 `/` 로 바꾸지 않으며 umask 도 0 으로 바꾸지 않습니다.
   예전 `-d` 로는 상대 경로 캐시·`CODEX_RUN_DIR` (배포 설정 모두 상대 경로) 가 `/` 기준이라 동작하지 않았습니다. 로그가 필요하면 `-l`.
4. **로그**: 요청 로그에 원문이 남지 않습니다 (`text_bytes=N`). 원문으로 검색하던 운영 절차가 있다면 uuid 로 바꿉니다.
   거부된 요청 값은 `Error: Invalid UUID format: ...` 처럼 INFO 로그에 이스케이프되어 남습니다 (예전에는 stderr 에 그대로).
5. **normal 백엔드**: 번역문이 8,191 바이트에서 잘리지 않고, 콜론(`10:30`, `http://`)이 보존됩니다. upstream 응답 본문 4 MB 초과,
   깨진 SSE 이벤트, `error` 이벤트는 `502` 입니다 (예전에는 부분 번역 200). 예전 바이너리로 저장된 **잘리거나 콜론이 빠진 번역**은
   캐시에 그대로 있으므로, normal 백엔드 캐시를 쓰고 있었다면 해당 항목을 지우는 것을 검토합니다 (`cache_tool`).
6. **trace**: `DEBUG=yes` 파일은 0600, API 키 가림. 기존 `trace/` 디렉터리의 권한은 바꾸지 않고 경고만 합니다.

**롤백:** 이전 바이너리로 돌아갈 수 있습니다 (형식 동일). 다만 따옴표 뒤 주석을 지운 설정은 이전 바이너리에서도 그대로 동작합니다.

## 14. 레거시 MCP 인터페이스 제거로 업그레이드

스키마·캐시 형식·exec 동작은 그대로입니다 ([PLAN_REMOVE_LEGACY_MCP.md](PLAN_REMOVE_LEGACY_MCP.md)).

1. 설정에 `CODEX_INTERFACE="mcp"` (대소문자 무관) 가 있으면 **codex 모드에서 기동하지 않습니다**:
   ``Error: CODEX_INTERFACE "mcp" was removed (codex-cli 0.154.0 dropped `codex mcp-server`); use CODEX_INTERFACE="exec" or delete the line``.
   `-d` 로 띄우면 실행한 쪽이 이 줄을 보여 주고 종료 코드 1 로 끝납니다. 그 줄을 지우거나 `"exec"` 로 바꿉니다.
2. normal 모드에서는 같은 줄이 경고 한 줄과 함께 무시됩니다.
3. 같은 키가 여러 줄이면 예전처럼 마지막 줄이 정합니다 (`mcp` 다음 `exec` 면 기동).
4. 배포 전에 확인: `grep -n 'CODEX_INTERFACE' <설정 파일>`. `"exec"` 이거나 줄이 없으면 할 일이 없습니다.

**롤백:** 이전 바이너리로 돌아갈 수 있습니다 (형식 동일).

## 15. timestamp 검증·SQLite 설정 수정으로 업그레이드 (2026-11-07)

스키마·캐시 형식은 그대로입니다 ([PLAN_CODE_ISSUES_4.md](PLAN_CODE_ISSUES_4.md)).

1. **timestamp**: 이제 날짜·시각 범위를 검사합니다. 불가능한 값 (`2026-02-30T...`, `T24:00:00`, 오프셋 `+24:00` 등) 과
   63바이트를 넘는 값은 **422** 입니다 (예전: 200, 긴 값은 63바이트로 잘린 채 응답). 캐시에 있는 원문이어도 마찬가지입니다.
   `T`·`Z` 는 지금처럼 대문자만 받습니다. 클라이언트가 보통의 RFC 3339 값 (예: `2026-11-07T01:02:03.456Z`) 을 보내면 할 일이 없습니다.
2. **SQLite**: `TRANS_CACHE_SQLITE_JOURNAL_MODE`·`TRANS_CACHE_SQLITE_SYNC` 가 이제 실제로 적용됩니다 (예전: 무시하고 항상 WAL/NORMAL).
   값이 `WAL`·`NORMAL` 이거나 키가 없으면 동작은 같고, 기동 로그에 `SQLite journal_mode=wal, synchronous=NORMAL` 한 줄이 늘어납니다.
   다른 값을 적어 두었다면 이제 그 값이 적용되므로 확인합니다: `grep -n 'TRANS_CACHE_SQLITE_' <설정 파일>`.
3. 프롬프트 파일을 못 찾을 때의 오류 로그가 `(null)` 대신 시도한 경로를 보여 줍니다.

**롤백:** 이전 바이너리로 돌아갈 수 있습니다 (형식 동일). WAL 이 아닌 모드를 썼다면 이전 바이너리는 DB 를 다시 WAL 로 바꿉니다.

## 16. 요청 본문 상한·normal 이스케이프 설정·codex 선착순·cache_tool 수정으로 업그레이드 (2026-11-07)

스키마·캐시 형식은 그대로입니다 ([PLAN_OUT_OF_SCOPE.md](PLAN_OUT_OF_SCOPE.md)).

1. **요청 본문 상한**: `/translate` 본문이 1 MiB (1,048,576 바이트) 를 넘으면 **413** (`errorCode` `PAYLOAD_TOO_LARGE`) 입니다 (예전: 상한 없이
   메모리에 누적). 정상 요청은 커야 60,000 바이트 정도라 보통은 할 일이 없습니다. 본문에 원시 NUL 바이트가 있으면 **422** 입니다
   (예전: NUL 뒤를 버리고 200). 앞단 프록시의 본문 제한은 그대로 두세요 — 연결 점유 (조금씩 오래 보내기) 는 이 상한으로 막지 못합니다.
2. **`NORMAL_UNESCAPE`** (normal 백엔드, 새 키): 기본 `yes` 는 지금과 같습니다 (번역문의 `\n`·`\t`·`\\` 등을 한 번 더 해제). `no` 로 바꾸면
   번역문 속 백슬래시가 그대로 남습니다. 바꾸기 전에 모델 출력에 글자 그대로의 `\n` 이 나오는지 확인하고, 바꾼 뒤에도 **이미 캐시된 번역은
   옛 처리 결과 그대로** 나오므로 필요하면 normal 캐시를 비웁니다 (README "normal 백엔드 후처리").
3. **codex 슬롯 대기**: 설정 변경 없음. 슬롯을 기다리는 요청이 들어온 순서대로 슬롯을 받습니다 (예전: 먼저 기다린 요청이 마감에 걸리고
   나중 요청이 슬롯을 가져갈 수 있었음). 상태 코드·마감은 같습니다.
4. **cache_tool**: `migrate`·`backfill-hints` (`--dry-run` 포함) 가 SQLite DB 의 journal 모드를 **바꾸지 않습니다** (예전: 언제나 WAL 로 바꿈).
   도구가 새로 만든 DB 는 SQLite 기본 `delete` 로 시작하고, 서버가 처음 열 때 서버 설정 (기본 WAL) 이 됩니다. 같은 SQLite 파일을 원본과
   대상으로 주면 (심볼릭·하드 링크, `dir/..` 포함) 거부합니다. WAL 이 아닌 DB 에 서버가 도는 중 도구를 쓰면 잠금을 기다릴 수 있습니다.

**롤백:** 이전 바이너리로 돌아갈 수 있습니다 (형식 동일). `NORMAL_UNESCAPE` 줄은 이전 바이너리가 모르는 키라 무시됩니다 (= `yes`).
이전 `cache_tool` 은 DB 를 다시 WAL 로 바꿉니다.

## 17. 실패 주입 시험·회복 수정으로 업그레이드 (2026-11-07)

설정·스키마·캐시 형식은 그대로이고 할 일은 없습니다 ([PLAN_FAULT_INJECTION.md](PLAN_FAULT_INJECTION.md)). 정상 동작은 같고
(프롬프트는 바이트 단위로 같음), 실패할 때만 다릅니다: 프롬프트 생성 중 메모리 부족은 `502` (예전: 자리표시자가 남은 프롬프트로 번역·캐시),
ROLLBACK 에 실패해 막힌 SQLite 캐시 연결은 다음 저장에서 회복 — 그 저장 하나는 건너뜀 (예전: 재시작 전까지 캐시 쓰기 실패).

**롤백:** 이전 바이너리로 돌아갈 수 있습니다.

## 18. HTTP 연결 상한·임시 디렉터리 로그로 업그레이드 (2026-11-09)

스키마·캐시 형식은 그대로이고, 설정을 바꾸지 않으면 할 일은 없습니다 ([PLAN_CONNECTION_LIMIT.md](PLAN_CONNECTION_LIMIT.md)).

1. **`HTTP_MAX_CONNECTIONS`** (새 키, 선택): 동시에 열린 HTTP 연결 수의 상한, 1~1020. 넣지 않으면 지금과 같습니다 (libmicrohttpd 기본 1020).
   상한에 닿은 연결은 거절되지 않고 커널 listen backlog 에서 기다립니다 — `/health` 도 기다리므로 헬스체크·프록시의 타임아웃을 함께
   보세요. 값을 정할 때는 서비스의 열 수 있는 파일 수 (`LimitNOFILE`, 쉘 `ulimit -n`; 1024 인 곳이 많음) 를 먼저 확인합니다. 그보다 크게
   두면 fd 가 먼저 바닥나 normal 번역이 503 으로 실패합니다. 계산법은 README "동시성". 정수가 아니거나 범위 밖이면 기동 오류입니다.
   ```conf
   # 동시에 열린 HTTP 연결 상한 (생략 = 1020)
   #HTTP_MAX_CONNECTIONS="200"
   ```
2. **새 로그 줄**: 기동 시 `HTTP connection limit: N (...)` 한 줄. 상한에 닿을 때·풀릴 때 INFO 두 줄 (60초에 한 번까지). codex 의 요청별 임시
   디렉터리를 만들지 못하거나 지우지 못하면 시도마다 INFO 한 줄 (예전: 일반 빌드에서는 남지 않음). 로그를 세는 감시가 있으면 확인하세요.

**롤백:** 이전 바이너리로 돌아갈 수 있습니다. `HTTP_MAX_CONNECTIONS` 줄은 이전 바이너리가 모르는 키라 무시됩니다 (= 1020).

## 19. 문제 해결

**빌드 실패 (라이브러리 없음)**
```bash
make check-deps          # 어떤 라이브러리가 빠졌는지 확인 후 설치
```

**설정 파일을 찾지 못함**
```bash
./transbasket -c /absolute/path/transbasket.conf
# 또는 TRANSBASKET_CONFIG 환경변수 사용
```

**포트 사용 중**
```bash
ss -ltnp | grep 8889     # 또는 lsof -i :8889
```

**normal 백엔드 API 연결 실패**
```bash
curl $OPENAI_BASE_URL/models
```

**codex 백엔드 실패 (502/504)**
```bash
grep -E "codex CLI|Warning" <로그>      # 기동 시 감지한 버전/인터페이스 문제
codex login status                     # 로그인 상태
ls -ld "${CODEX_HOME:-$HOME/.codex}"   # 쓰기 가능 여부
ls -ld <CODEX_RUN_DIR>                 # 임시 cwd 루트 존재/권한
```
`502` 응답의 `errorMessage` 에는 codex 의 마지막 `ERROR:` 줄이 담깁니다(예: 지원하지 않는 모델).
`DEBUG=yes` 이면 요청마다 `./trace/<timestamp>_<uuid>.codex.txt` 에 프롬프트/결과/codex stderr 트레이스가 남습니다
(파일 0600, 같은 이름은 `-1` 등을 붙임, normal 의 curl trace 는 요청 본문만 담고 API 키는 `<redacted>`). 원문 (codex 는 번역문도) 평문이 저장되고
파일이 정리 없이 쌓이므로 운영 환경에서는 `DEBUG=no` 를 권장합니다.

**메모리 누수 점검**
```bash
valgrind --leak-check=full --show-leak-kinds=all ./transbasket
```
