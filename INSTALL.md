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

## 8. 문제 해결

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
