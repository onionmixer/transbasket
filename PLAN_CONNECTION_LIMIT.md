# HTTP 연결 상한과 임시 작업 디렉터리 실패 로그 계획

작성 2026-11-09. 작업 위치: worktree `../transbasket-connlimit`, 브랜치 `connlimit` (main `2d44be9`).
요청: "임시 작업 디렉터리 실패 로그와 동시 연결 상한" — 연결 상한은 transbasket HTTP 서버 (`http_server.c`, libmicrohttpd)
에 둔다 (사용자 선택 1번: 여기에 두면 normal·codex 와 dkfs·pge4 같은 배포가 모두 적용받는다). 원칙: 계산·기대값은 python,
codex 는 코딩 전 계획 검토 입력일 뿐, 별도 worktree, 모든 기준 통과 후 main fast-forward. `파일:줄` 은 `2d44be9` 기준.

## 1. 지금 상태 (원문·실측)

### 1.1 연결 상한

- `MHD_start_daemon()` (`http_server.c:697-707`) 에 `MHD_OPTION_CONNECTION_LIMIT` 가 없다. 플래그는
  `MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG` (`:684`) — `MHD_USE_POLL`/`EPOLL`
  없음, 즉 select.
- libmicrohttpd 0.9.75 원본 (ftp.gnu.org tarball, sha256 `9278907a…e3bb`; `src/include/microhttpd.h` 가 설치된
  `/usr/include/microhttpd.h` 와 바이트 동일):
  - 기본 상한 `MHD_MAX_CONNECTIONS_DEFAULT = FD_SETSIZE - 4` (`daemon.c:78`), 이 호스트 FD_SETSIZE 1024 (C 로 출력) → **1020**.
  - select 모드에서 fd **번호**가 FD_SETSIZE 이상인 새 소켓은 닫는다 (`daemon.c:2984-2998`). 연결 수 상한과 fd 번호 한도는 다른
    것이고, 다른 fd (listen, ITC, 캐시, upstream 소켓, codex 파이프) 가 번호를 먹으므로 실제 수용량은 1020 보다 작다.
  - `MHD_OPTION_CONNECTION_LIMIT` 는 값을 그대로 넣는다 — 범위 검사·clamp 없음, 0 이면 아무것도 안 받음 (`daemon.c:5663-5666`).
  - accept 가 `EMFILE` 같은 자원 부족으로 실패하면 `at_limit` 를 세우고 accept 를 잠시 멈춘다 (`daemon.c:3655-3685`),
    연결 하나가 정리되면 푼다 (`:3853-3854`).
- ITC: thread-per-connection 에 `MHD_OPTION_NOTIFY_COMPLETED` 가 있으면 ITC 를 켠다 (`daemon.c:6593-6595`) — 우리 서버는
  넘긴다 (`http_server.c:704`). ITC 가 있으면 상한이나 `at_limit` 일 때 listen 소켓을 select 집합에서 뺀다 (`daemon.c:4279-4291`),
  accept 는 listen 소켓이 읽기 가능일 때만 (`:4043-4048`). → **상한을 넘은 연결은 끊기지 않고 커널 backlog 에서 기다린다.**
  `new_connection_prepare_()` 의 "Closing inbound connection" 검사 (`:2460-2474`) 는 남아 있지만, 이 모드의 실측에서는 닿지 않았다
  (로그 없음).
- 연결 수는 연결 스레드가 끝나며 자신을 cleanup 목록에 넣고 ITC 를 깨우면 (`connection.c:4258-4305`), polling 스레드의
  `MHD_cleanup_connections()` 가 join → CLOSED 알림 → 정리 → `connections--`·`at_limit=false` 순으로 줄인다 (`daemon.c:3768-3854`).
- 실측 (probe `scratchpad/connlimit/mhd_probe2.c` = 서버와 같은 플래그 + `NOTIFY_COMPLETED` + 로거 + 120 s; 그리고 실제 `transbasket`):
  - **상한 2, /slow (1.5 s) 4개 + 0.3 s 뒤 /fast**: 모두 200 — 2개 1.5 s, 나머지 3개 3.0 s. MHD 로그 없음.
    상한 4, 10개 묶음 3회: 모두 200, 묶음 중 /fast 2.5 s, 끝난 직후 즉시 200. → **대기 후 처리, 거절 없음, 로그 없음.**
  - ⚠ 처음 probe (`mhd_probe.c`) 는 `NOTIFY_COMPLETED` 를 빠뜨려 ITC 가 없었고, 그래서 "즉시 reset" 으로 잘못 쟀다 (codex 지적, 원문·재측정 확인).
  - fd 고갈 (`ulimit -n 64`, 80 연결): probe 는 accept 를 멈췄다가 연결이 끝나며 다시 받아 80개 모두 200 (2~3 s), 그 뒤 정상.
    실제 서버 (normal, upstream 1.5 s): 200 48건, **503 32건** (upstream 소켓을 못 열어 재시도 백오프 2+4 s 후 6~9 s 에 503),
    그 뒤 `/health` 정상. idle 연결 80개로 fd 를 채우면 그동안 `/health` 시간 초과, 닫으면 회복. 연결이 0개일 때의 자원 부족은
    `at_limit` 를 세우지 않는다 (`daemon.c:3653-3667`), upstream fd 가 풀리는 것만으로는 `at_limit` 가 안 풀린다 (연결 정리 때만).
  - ⚠ "fd 고갈 뒤 영구 정지" 로 보인 것은 **내 시험의 인공물** (probe stderr 를 PIPE 로 받고 읽지 않음). MHD 결함으로 적지 않는다.
  - **backlog 에서 포기한 요청도 처리된다**: 상한 1, 앞 연결이 2 s 붙잡은 동안 두 번째 클라이언트가 요청을 보내고 0.5 s 뒤 닫음 →
    핸들러가 2.0 s 에 그 요청을 실행 (`mhd_probe3.c`, `abandon.py`). transbasket 이면 떠난 클라이언트의 번역이 나중에 돈다
    (결과는 캐시에 남지만 엔진 호출은 쓰인다). MHD 무활동 타임아웃 (120 s) 은 accept 뒤부터라 backlog 대기에는 안 걸린다
    (`microhttpd.h:1578-1585`). listen backlog 는 `SOMAXCONN` (`daemon.c:6524-6527`), 커널 `net.core.somaxconn` 4096.
- 운영 값: dkfs (PID 1015134) `open files` soft 1024 / hard 1048576, 지금 fd 8개. 즉 지금 상한은 사실상 "fd 가 바닥나는 지점"
  (1020 근처) 이고, 거기서는 거절이 아니라 번역 실패 (normal 503) 와 지연이 난다.
- 이미 있는 한도와의 관계: `INFLIGHT_MAX_KEYS` (기본 `MAX_WORKERS`) 는 서로 다른 번역 수, `INFLIGHT_MAX_WAITERS` (256) 는
  따라오는 요청 수를 막고 넘으면 503 + `Retry-After`. 캐시 적중·`/health`·keep-alive idle 연결은 이 한도에 안 걸린다.
  연결 상한은 그 바깥 — 연결 스레드와 fd 의 상한이다.

### 1.2 임시 작업 디렉터리 실패 로그

- `backend_codex.c:253-258`: `mkdtemp` 실패 → `LOG_DEBUG` (릴리스 빌드에는 컴파일되지 않음, `include/utils.h:11-15`) →
  codex 는 `-C`/`-o`/`--output-schema` 없이 서버 cwd 에서 실행, auto+model 은 plain 출력·script 힌트로 내려간다. **릴리스
  빌드는 이를 전혀 남기지 않는다** (`tests/test_fault_server.py:167` 이 그렇게 단정).
- 같은 함수의 schema 파일 실패는 이미 요청마다 INFO (`:268`, "cannot write the output schema").
- 정리 실패: schema `unlink` 실패 (`:339-341`) 와 임시 디렉터리 `rmdir` 실패 (`:342-346`) 도 `LOG_DEBUG` — `rmdir` 이 실패하면
  `CODEX_RUN_DIR` 에 `tb-XXXXXX` 가 남아 쌓인다.

## 2. 변경

### 2.1 임시 작업 디렉터리 로그 (결정 불필요)

- `mkdtemp` 실패를 **실패한 backend 시도마다 INFO** 로 (캐시 적중·병합 follower 는 `mkdtemp` 를 부르지 않는다):
  `[uuid] cannot create a temp directory under <CODEX_RUN_DIR> (<strerror>); running codex in the server's working directory
  without -C/-o/--output-schema`. errno 는 실패 직후 지역 변수에 저장해서 쓴다.
  이유: 같은 함수의 schema 실패가 이미 요청마다 INFO (`:268`) 이고, codex 요청은 어차피 요청마다 INFO 를 여러 줄 남긴다.
- `rmdir` 실패도 INFO, 결과를 단정하지 않는 문구: `[uuid] cannot remove temp directory <path> (<strerror>)` (ENOENT 면 남은 것이
  없을 수도 있다). schema `unlink` 실패는 DEBUG 로 둔다.
- `CODEX_RUN_DIR` 은 설정 값이라 `log_escape()` 대상이 아니다 (CLAUDE.md "Logs": 원격·자식 문자열만). `<path>` 는 `mkdtemp` 결과.
- 이 호스트 glibc 2.35 의 `strerror()` 는 스레드 안전 (2.32 부터) — 기존 코드와 같게 쓴다.
- 손대지 않음 (기록만): `write_schema_file()` 의 `w == 0` 경로는 errno 를 설정하지 않은 채 -1 을 돌려준다 (`backend_codex.c:150`).

### 2.2 HTTP 연결 상한

- 새 키 **`HTTP_MAX_CONNECTIONS`**: `Config.http_max_connections`, 0 = 미설정 (−1 = 잘못된 값과 구분). `positive_integer()`
  (`config_loader.c:261`) 로 읽는다. 읽힌 값이 정수가 아니거나 1..1020 밖이면 `validate_config()` 가 **두 backend 모두에서**
  기동을 멈춘다. 같은 키가 두 번이면 마지막 줄이 이긴다 (`INFLIGHT_*` 와 같음). 너무 긴 줄·파싱 안 되는 줄은 다른 키처럼 경고 후
  무시된다 (`config_loader.c:512-550`, `tests/test_config_limits.py` 가 그 동작을 요구) — 그 경우 미설정과 같다.
- 1020 은 **이 프로그램의 정책**: MHD 기본값 (POSIX, FD_SETSIZE−4) 이고 MHD 는 범위를 검사하지 않는다. 수용량 보장은 아니다 —
  fd 번호 1024 이상인 소켓은 select 모드에서 닫히고, 다른 fd 가 번호를 먹는다.
- **없으면 지금과 같다**: 옵션을 넘기지 않아 MHD 기본 1020.
- 넘길 때는 `MHD_OPTION_CONNECTION_LIMIT, (unsigned int)n` 을 `MHD_OPTION_EXTERNAL_LOGGER` 다음 (로거는 첫 옵션, CLAUDE.md
  "LISTEN address") 에 붙인다. 가변 인자라 `MHD_OPTION_ARRAY` 로 하나의 호출을 만든다.
- 하나의 daemon 전체 상한이다: dual-stack (`::`) 이면 IPv4·IPv6 가 한 상한을 나눠 쓴다. 주소별 상한 (`PER_IP`) 은 쓰지 않는다
  (앞단 gateway·프록시가 한 주소로 보이므로).
- 기동 로그 (한 줄): `HTTP connection limit: N (HTTP_MAX_CONNECTIONS)` 또는 `HTTP connection limit: 1020 (libmicrohttpd default)`.
  `-d` 에서도 시작 보고 전 줄이라 `-l` 파일에 들어간다 (`log_set_tee()`).
- 넘친 연결: MHD 그대로 backlog 대기 (결정 1 = A). 우리 쪽 코드는 거절하지 않는다.
- 상한 도달 기록 (결정 3): `MHD_OPTION_NOTIFY_CONNECTION` 으로 열린 연결 수를 센다 (STARTED +1, CLOSED −1; MHD 안에서
  부르는 곳은 이 셋뿐 — `daemon.c:2811`, `:2898`, `:3799`; 등록해도 플래그는 안 바뀐다). 실효 상한 L (설정 값, 없으면 1020) 에서:
  - STARTED 로 L 에 닿으면: 마지막 "reached" 줄에서 60 s 이상 지났으면 INFO
    `HTTP connection limit L reached; new connections wait in the listen backlog` (그 사이 숨긴 도달이 있으면
    `(K more times since the last report)` 를 붙인다), 아니면 K 만 늘린다.
  - 그 도달을 기록했으면 CLOSED 로 L 아래로 내려갈 때 INFO `HTTP connection count below the limit again after X.Y s`.
    기록 안 한 도달은 풀림도 남기지 않는다 (줄이 늘 짝을 이룸).
  - 상태는 mutex 하나로 보호 (STARTED·CLOSED 는 이 모드에서 polling 스레드가 부르지만, 정지 경로까지 단정하지 않는다). 시간은 monotonic.
  - 로그 호출은 그 콜백 안 (polling 스레드) 에서 — 다른 LOG 처럼 stdout 에 동기 쓰기.
  - fd 고갈 (`at_limit`) 은 이 기록 대상이 아니다 (연결 수가 L 에 못 미쳐도 생김) — 문서에 적는다.

## 3. 불변식 (기대값)

1. `HTTP_MAX_CONNECTIONS` 없음 → MHD 에 옵션을 넘기지 않음; 동작·로그가 지금과 같음 (기동 줄 하나 추가 외).
2. `HTTP_MAX_CONNECTIONS=N` → 동시에 열린 연결은 N 개까지. 넘친 연결은 backlog 에서 기다린다 (503 없음). 연결이
   정리되면 (polling 스레드의 cleanup) 다시 받는다 — 시험은 고정 지연을 가정하지 않고 제한 시간 안의 회복을 본다.
3. keep-alive idle 연결도 하나로 센다. 120 s 무활동이면 MHD 가 닫는다 (`http_server.c:705`).
4. 값 검증: `0`, `-1`, `+1`, `abc`, `" 1"`, `"1 "`, `1021`, `99999999999`, `KEY=`, `KEY=""` → 기동 실패 (exit 1, 바인드 안 함, `-d` 면
   launcher exit 1). `1`, `1020`, `0010` → 기동 (positive_integer 는 앞자리 0 을 받는다 — 지금 키들과 같음). 무효→유효 순이면 기동,
   유효→무효 순이면 실패.
5. `mkdtemp` 실패한 시도마다 그 문구 정확히 한 줄, 성공 시도에는 없음; `rmdir` 이 ENOTEMPTY 로 실패하면 그 문구 한 줄과 디렉터리가 남음.
6. 다른 경로의 로그·응답은 같음 — parity 에서 새 기동 줄 하나만 허용 (그 줄이 새 빌드에 정확히 한 번 있는지도 따로 확인).

## 4. 시험

- `make test-conn-watch` (새, C): 상태 기계에 시각을 넣어 — 첫 도달은 늘 기록, 59,999 ms 숨김 / 60,000 ms 기록, K 세기와 초기화,
  숨긴 도달은 풀림도 없음, 정지 표시, 0 아래로 안 내려감, limit 1.
- `make test-connection-limit` (새, python, 실제 서버, 오프라인 fake upstream; 미설정 동작의 이전 빌드 비교는 parity 가 맡는다):
  - 미설정: 6개 동시 붙잡힘 → 모두 처리, 기동 줄 "libmicrohttpd default".
  - N=2: fake upstream 이 요청을 받은 것을 **서버 쪽 이정표**로 확인한 뒤 (TCP 연결 성공은 accept 를 뜻하지 않음) 세 번째 연결의
    운명을 본다: 붙잡힌 동안 응답 없음, 놓아 주면 **그 연결 자신이** 200.
  - `/health`·캐시 적중도 상한에 묶인다: 꽉 찬 동안 응답 없음, 풀리면 그 요청이 200. 상한 아래에서는 즉시.
  - idle keep-alive 연결이 자리를 차지하고, 닫으면 풀린다. 첫 요청 전에 닫힌 연결도 자리를 돌려준다.
  - dual-stack `::` 에서 IPv4 와 IPv6 연결이 한 상한을 나눠 쓴다.
  - 값 검증 표 (불변식 4), 전경과 `-d`.
  - fd 예산: `ulimit -n 64`, `INFLIGHT_MAX_KEYS=1000` (번역 입장 503 이 끼지 않게), 서로 다른 원문 80개, upstream 1 s.
    기동 직후 fd 수 b 를 재고 N = ⌊(64 − b − 8) / 4⌋ (python). 상한 N → 80개 모두 200. 같은 부하를 미설정으로 → 503 이 생김
    (이 시험이 fd 를 실제로 바닥내는지의 기준; 2026-11-09 실측 10~32건, 7회).
    ⚠ 처음엔 연결 하나에 fd 2개 (클라이언트 + upstream) 로 셈해 N = 25 였고, 단독 실행은 통과했지만 전체 회귀에서 503 이 1건 (ASan 8건)
    났다. 503 본문은 `Couldn't connect to server`; 최대 fd 64 를 소켓별로 나눠 보니 UNIX socketpair 29개 — libcurl 7.81 이 전송마다
    socketpair 를 연다. 연결 하나 = 클라이언트 + upstream + socketpair 2 = **4개**. N = 12 에서 최대 fd 53 = 5 + 12×4 (릴리스 4회·ASan 2회
    모두 80/80).
- `test-fault-server` F1: `"mkdtemp" not in text` 부분 문자열 검사를 바꿔, 새 문구가 실패한 두 시도에 정확히 두 줄, 기준·회복 시도에는
  없음. F1b: fake codex 가 **`-C` 인자의 디렉터리**에 파일을 남기게 해 `rmdir` ENOTEMPTY → 문구 한 줄 + 디렉터리가 남음. root 면 F1 은
  지금처럼 건너뜀.
- 도달 기록: N=2 에서 두 연결을 붙잡으면 "reached" 한 줄, 놓으면 "below ... after X s" 한 줄 — 순서와 짝을 보고, X 는 python 이
  잰 [도달 확인 시각, 놓은 뒤 응답 시각] 구간을 여유 있게 감싸는지만 본다 (STARTED 는 스레드 생성 전, CLOSED 는 join 뒤라 X 에
  준비·정리 시간이 들어감). 60 s 안에 다시 닿으면 줄 없음; 첫 기록 60 s 뒤에 닿으면 "(1 more times ...)" 줄과 짝 (실제 대기 ~65 s,
  그 사이 다른 연결 없음). 상한에서 SIGTERM → "below ... (during shutdown)". 미설정 (L=1020) 에서는 평소 부하에 아무 줄도 없음.
- 변이: 옵션 미전달, 상한 N+1 전달, 검증 상한 1021 허용, 0 허용, mkdtemp·rmdir 줄 DEBUG 로 되돌림, 도달 판정 off-by-one, 60 s 억제
  제거, 짝 규칙 제거 — 모두 잡혀야 함.
- 회귀: 기존 33 타깃 (`test-coalescing` 의 10 연결 시험은 기본값 그대로라 영향 없음) + parity (`PARITY_OLD_BIN=` 2d44be9, 새 기동
  줄만 허용) + prompt regression + ASan.

## 5. 결정 (2026-11-09 사용자: 세 가지 모두 권장안 — 1 = A, 2 = 1020 유지, 3 = 닿을 때·풀릴 때 기록)

1. **상한을 넘은 연결**:
   - A (권장): MHD 그대로 — **backlog 에서 기다렸다가 자리가 나면 처리** (실측). 코드는 옵션 하나. 클라이언트는 자기 타임아웃까지
     기다리고, 포기한 요청도 나중에 처리된다 (엔진 호출 낭비 가능). `/health` 도 기다린다.
   - B: 앱이 연결 수를 직접 센다 (`MHD_OPTION_NOTIFY_CONNECTION`: STARTED 는 polling 스레드에서 연결 스레드 만들기 전,
     CLOSED 는 cleanup 의 join 뒤 — `daemon.c:2811-2824`, `:3799-3803`). N 을 넘은 연결은 `503 SERVER_BUSY` + `Retry-After: 5` +
     `Connection: close`, POST 본문은 1 MiB 까지 모으지 않고 바로 버리며 마지막 호출에서 응답 (첫 호출 응답은 본문 읽기를 멈춰
     reset 위험 — `connection.c:5257-5263`). MHD 상한 H > N 을 따로 두고 H 를 넘으면 다시 backlog 대기 — **N..H 사이만 503 이 보장**,
     N 의 최대는 1020 보다 작아야 한다. 느린 업로더는 503 자리도 오래 잡는다. `request_completed()` 가 모든 `con_cls` 를
     `RequestBody` 로 여기는 것 (`http_server.c:471-481`) 등 상태 분리가 필요 — 코드·시험이 크게 는다.
2. **기본값**: 미설정 = 지금처럼 1020 (권장) vs fd 한도에서 계산한 값. fd 수요는 기동 fd + 연결 + backend 동시성 (normal upstream
   소켓, codex 파이프 3개) 으로 나뉘어 상수 하나로 못 맞춘다 (codex 지적) — 그래서 문서에 예산 계산법과 예시를 두는 쪽을 권한다.
3. **상한 도달 기록**: A 에서는 MHD 도 아무것도 남기지 않는다 (실측). 연결 수를 세어 (`NOTIFY_CONNECTION`) 상한에 닿을 때 INFO
   `HTTP connection limit N reached; new connections wait in the listen backlog` 와 풀릴 때 `... below the limit again after X s` 를
   남길지 (권장, 닿을 때 줄은 60 s 에 한 번까지), 남기지 않을지.

## 6. 문서

README "한계와 보장"·설정 표, INSTALL (설정 키·업그레이드 메모), CLAUDE.md (Threading Model 의 ⚠ 문단, Size limits 표,
새 불변식 절, Fault-injection 절의 "A release build logs nothing" 문장), 템플릿 `transbasket.conf` 에 주석 처리된 예시,
TEST_RESULTS.

## 7. codex 계획 검토 판정 (코딩 전)

codex (read-only, 2026-11-09) 16건. 각 건은 원문을 열거나 다시 재서 판정했다 (`M/` = libmicrohttpd-0.9.75/src/microhttpd).

| # | codex 주장 | 내 검증 | 판정 |
|---|---|---|---|
| 1 | 우리 옵션 (`NOTIFY_COMPLETED`) 은 ITC 를 켜고, 그러면 상한에서 listen 소켓을 빼서 backlog 대기 — 내 probe 는 그 옵션이 없었다 | `M/daemon.c:6593-6595` 열어 확인, `mhd_probe.c:23-27` 에 옵션 없음 확인, `mhd_probe2.c` (옵션 추가) 로 재측정: 상한 2 에서 넘친 3개 3.0 s 에 200, 로그 없음; 상한 4·10개 3회 모두 200 | ✅ 채택 — 내 실측이 틀렸음. §1.1·§5 다시 씀 |
| 2 | 연결 수는 polling 스레드 cleanup 에서 join 뒤 줄어든다, 고정 지연 아님 | `M/connection.c:4258-4305` (cleanup 목록 + ITC), `M/daemon.c:3768-3803` (join, CLOSED) 열어 확인 | ✅ 채택 — 불변식 2·시험을 "제한 시간 안의 회복" 으로 |
| 3 | 자원 부족 errno 묶음, 연결 0개면 `at_limit` 안 세움, 연결 정리 때만 풀림 | `M/daemon.c:3653-3685`, `:3853-3854` 열어 확인 (`mhd_sockets.h` 목록은 안 열었음 — 계획에 옮기지 않음) | ✅ 채택 (목록 제외) |
| 4 | 1020 은 기본값이지 MHD 가 강제하는 최대가 아님, 0 이면 0 | `M/daemon.c:5663-5666` (그대로 대입), `:78` 열어 확인 | ✅ 채택 — 근거 문구 바꿈 |
| 5 | per-IP·thread pool 은 별개, pool 은 thread-per-connection 과 같이 못 씀 | `M/daemon.c:5766-5773` 열어 확인 | ✅ 채택 — 문서에 "daemon 전체 상한" 명시 |
| 6 | B 의 알림: STARTED 는 polling 스레드에서 연결 스레드 생성 전, CLOSED 는 join 뒤 | `M/daemon.c:2738-2830` 에서 STARTED 호출·`MHD_create_named_thread_` 순서, `:3799-3803` CLOSED 확인. "CLOSED without STARTED 없음" 은 전수 확인 안 함 — 계획에 옮기지 않음 | ⚖️ 부분 — 확인한 부분만 §5 |
| 7 | B 는 N 을 넘은 모든 연결에 503 을 보장 못 함 (H 에서 다시 backlog) | 1번 실측 (ITC 모드의 상한 = backlog 대기) 에서 따라 나옴 | ✅ 채택 — §5 B 에 명시 |
| 8 | 본문 버리기에 413 경로를 그대로 못 씀, `request_completed()` 가 `con_cls` 를 모두 `RequestBody` 로 여김, 응답 helper 에 `Connection: close` 없음 | `http_server.c:183-192`, `:226-275`, `:471-481`, `:53-74` (Retry-After 만), `M/connection.c:5257-5263` 열어 확인 | ✅ 채택 — §5 B |
| 9 | 120 s 는 무활동 타임아웃, backlog 대기에는 안 걸림 | `/usr/include/microhttpd.h` OPTION_CONNECTION_TIMEOUT 주석 (1578-1585) 은 앞서 열어 봄 | ✅ 채택 |
| 10 | 숫자 파싱 계약 (빈 값, +1, 공백, 앞자리 0, 중복 키) 을 명시하라 | `config_loader.c:261-267` (positive_integer), `:666-673` 확인 | ✅ 채택 — 불변식 4 |
| 11 | 너무 긴 줄·파싱 실패 줄은 무시되어 기본값이 남는다 | `config_loader.c:512-550` 열어 확인 | ✅ 채택 — 다른 키와 같게 두고 문서화 (LISTEN 식 특수 처리 안 함) |
| 12 | "64 fd·32 연결이면 503 없음" 시험은 보장이 아니다 (upstream 30개) | 산술: 32 + 30 = 62 (기동 fd 5개 별도) — python 으로 계산 | ✅ 채택 — `INFLIGHT_MAX_KEYS=4` 와 실측 기동 fd 로 예산 계산 |
| 13 | errno 를 즉시 저장, glibc 2.32+ strerror 스레드 안전, `write_schema_file` errno | `backend_codex.c:140-152` 확인 (`w == 0` 이면 errno 미설정); `ldd --version` 2.35 | ✅ 채택 (schema errno 는 기록만) |
| 14 | rmdir 문구가 결과를 단정함, F1b 는 `-C` 디렉터리에 파일을 만들어야 함 | `tests/test_fault_server.py:39-49` (fake 가 `-C` 를 따르지 않음) 확인 | ✅ 채택 |
| 15 | parity 가 새 기동 줄에서 깨짐, F1 은 부분 문자열 검사라 새 문구를 못 잡음, CLAUDE.md 문장 | `tests/test_backend_parity.py:174-184`, `:311-316`, `tests/test_fault_server.py:167`, `CLAUDE.md:370-372` 확인 | ✅ 채택 |
| 16 | `-d`, `/health`·캐시 적중, dual-stack, 기존 부하 시험, 서버 쪽 이정표 | `http_server.c:684-707` (daemon 하나), `tests/test_request_coalescing.py` 10 연결은 기본값이라 영향 없음 | ✅ 채택 — §4 |

내가 틀린 것: §1.1 의 첫 실측 ("넘친 연결은 즉시 reset") — 서버와 다른 옵션의 probe 로 쟀다.

### 7.2 2차 검토 (기록 설계, 2026-11-09)

| # | codex 주장 | 내 검증 | 판정 |
|---|---|---|---|
| 1 | 우리 수는 STARTED 에서 MHD 와 같고, CLOSED 에서는 MHD 의 `connections--` 보다 먼저 준다 (그 사이 accept 없음) | `M/daemon.c:2784-2830` (++·unlock·STARTED), `:3768-3805` (CLOSED 는 join 뒤), `:2885-2915` 직접 열어 확인 | ✅ 채택 — "점유 슬롯" 으로 표현 |
| 2 | 상한·fd 검사 등으로 거절된 연결은 STARTED 를 안 받는다; 스레드 생성 실패는 STARTED·CLOSED 짝 | `:2786` 의 확정 검사가 STARTED 앞, `:2898` CLOSED 경로 확인 (`:2497`·`:2513`·`:2541`·`:2770` 은 안 열었음 — 옮기지 않음) | ⚖️ 부분 — 스레드 실패 때의 짧은 도달·풀림 가능성은 문서에 |
| 3 | 세 알림 모두 polling 스레드, 정지 때도 (`close_all_connections`), 콜백 중 cleanup mutex 는 안 잡혀 있음; 막힌 로그는 accept·정리를 멈춤 | `:5394-5400`, `:7605-7645` (unlock 후 cleanup), `:3768-3805` 확인 | ✅ 채택 — 로그는 watch mutex 밖에서, 위험은 문서에 |
| 4 | 콜백 시그니처 맞음, 위치 제약 없음, ARRAY 는 `value` (intptr_t), 상태는 `MHD_start_daemon` 전에 초기화 | `microhttpd.h:2495-2499` typedef, `:1725-1735`, `M/daemon.c:6082-6093`·`:6147-6157` 확인 | ✅ 채택 |
| 5 | 첫 도달은 늘 기록 (`reported_once` 따로), 숨긴 도달만 K, 숨김·풀림은 마지막 보고 시각을 안 바꿈, monotonic | 설계 규칙 — `conn_watch.c` 에 그대로 구현, `test-conn-watch` 로 확인 | ✅ 채택 |
| 6 | 정지 때의 풀림 줄이 회복처럼 보임 → "(during shutdown)" | `http_server.c` stop 경로: `inflight_shutdown` → `MHD_stop_daemon` 확인 | ✅ 채택 — stop 첫머리에 표시 |
| 7 | fd 고갈은 기본 L 의 도달 줄을 만들지 않고, 오히려 못 남기게 함 — 줄이 없다고 여유가 있는 것은 아님 | `M/daemon.c:3653-3685` (at_limit 는 알림 없음) | ✅ 채택 — 문서에 |
| 8 | §3·§4 가 아직 "503" 을 허용 — 결정 A 와 어긋남; fd 비교에 번역 입장 503 이 섞일 수 있음 | 계획 해당 줄 다시 읽음; `INFLIGHT_MAX_KEYS` 입장 503 (`http_server.c:366-372`) 확인 | ✅ 채택 — §3·§4 고침 |
| 9 | X 정밀 비교 금지 (준비·정리 시간 포함), K=1 시험은 다른 연결 없이, 상태 기계 단위 시험 | 스모크 실측: 붙잡은 1.0 s 에 "after 1.0 s" | ✅ 채택 — 구간 포함만 검사, 단위 시험 추가 |
