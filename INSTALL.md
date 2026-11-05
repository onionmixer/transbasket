# Transbasket Installation Guide

> 이 문서는 **C 구현** 기준입니다. 초기 Python POC(`poc/`)는 폐기되었고 저장소에서 제거되었습니다.
> 설정 키 전체 목록과 API 스펙은 `README.md`, codex 백엔드 설계는 `PLAN_MODEL_TYPE_CODEX_MCP.md` 를 참고하세요.

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

상대 경로는 설정 파일 기준으로 해석되므로, 다른 위치에서 실행할 때는 절대 경로 사용을 권장합니다.

## 4. 실행

```bash
./transbasket                                   # transbasket.local.conf → 없으면 transbasket.conf
./transbasket -c /etc/transbasket/transbasket.conf
./transbasket -c transbasket_codex.conf         # codex 백엔드
./transbasket -w 20                             # 워커 스레드 수 지정
MAX_WORKERS=20 ./transbasket                    # 환경변수로도 지정 가능
./transbasket -d -c /etc/transbasket.conf       # 데몬 모드
```

옵션: `-c/--config`, `-p/--prompt`, `-r/--role`, `-w/--workers`, `-d/--daemon`, `-h/--help`.
환경변수: `TRANSBASKET_CONFIG`, `MAX_WORKERS`. 워커 수 권장값은 `2 * CPU_CORES + 1` 입니다.

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

응답은 요청의 `uuid` / `timestamp` 를 그대로 보존합니다.

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
2026-11-03의 오프라인 검증은 9개 통과했고 실제 TranslateGemma 호출은 503으로 실패했습니다.
배포 후 실제 대상 모델에서 자동 감지·번역 결과를 확인하세요. 세부 검증 기록은 [TEST_RESULTS.md](TEST_RESULTS.md)에 있습니다.

## 6. codex 백엔드 추가 준비

```bash
codex --version          # CLI 설치 확인 (exec: 0.166.0, 레거시 mcp: 0.148.0 에서 검증)
codex login status       # 서버를 '실행하는 계정'이 로그인되어 있어야 함
codex exec --help        # 기본 인터페이스(exec)가 있는지
codex mcp list           # 이 계정의 MCP 서버 - 있으면 아래 CODEX_IGNORE_USER_CONFIG 참고
```

체크 포인트:

- **codex-cli 0.154.0 부터 `codex mcp-server` 가 없습니다.** 기본값 `CODEX_INTERFACE="exec"` 를
  유지하세요. `mcp` 는 그 이전 버전에 고정된 호스트에서만 동작합니다. 기동 로그의
  `codex CLI: codex-cli X.Y.Z (interface=...)` 줄과 그 아래 `Warning:` 으로 확인할 수 있습니다.
- 서비스 계정의 `config.toml` 에 MCP 서버/플러그인이 있으면 매 번역마다 함께 기동되고, 그 도구는
  read-only 샌드박스 밖입니다. 번역 전용 계정을 쓰거나 `CODEX_IGNORE_USER_CONFIG="true"` +
  `CODEX_MODEL` 을 지정하세요.

- 서비스 계정이 쓰기 가능한 `CODEX_HOME`(기본 `~/.codex`)을 가져야 핸드셰이크가 성공합니다.
- `CODEX_RUN_DIR`(요청별 임시 cwd 루트, 기본 `/var/lib/transbasket/run`)을 **미리 생성**하고 0700 으로 두세요.
  없으면 `mkdtemp` 가 실패해 codex 가 서버의 작업 디렉터리에서 실행됩니다.
- **`TRANS_CACHE_THRESHOLD="1"` 필수** — codex 출력은 비결정적이라 임계값이 2 이상이면
  캐시 count 가 계속 1 로 리셋되어 캐시가 무효화되고 매 요청마다 codex 를 호출합니다.
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
#INFLIGHT_MAX_KEYS="30"          # 생략하면 MAX_WORKERS 사용
INFLIGHT_MAX_WAITERS="256"
INFLIGHT_MAX_WAITERS_PER_KEY="64"
```

전체 예산은 1~110초이며 슬롯 대기·MCP 초기화·재시도·백오프·호출에 함께 적용합니다.
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
(예전에는 스택 버퍼를 넘어 기동 시 abort 했습니다).

## 11. 문제 해결

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
`codex MCP initialize failed` 라면 `CODEX_INTERFACE="mcp"` 로 신버전 codex 를 구동한 것입니다.
`DEBUG=yes` 이면 요청마다 `./trace/<timestamp>_<uuid>.codex.txt` 에 프롬프트/결과/codex stderr 트레이스가 남습니다.
운영 환경에서는 파일이 무한히 쌓이고 원문·번역문 평문이 저장되므로 `DEBUG=no` 를 권장합니다.

**메모리 누수 점검**
```bash
valgrind --leak-check=full --show-leak-kinds=all ./transbasket
```
