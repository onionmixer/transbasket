# "범위 밖으로 둔 것" 4건 수정 계획

작성 2026-11-07. 작업 위치: worktree `../transbasket-oos`, 브랜치 `fix/out-of-scope` (main `1348a53`).
요청: "범위 밖으로 둔 것 작업을 진행". 원칙: 계산·기대값은 python, codex 는 입력일 뿐 (코딩 전 계획 검토), 별도 worktree, 모든 기준 통과 후
main fast-forward. `파일:줄` 은 `1348a53` 기준.

## A. 요청 본문 누적 상한 없음

현재 (`http_server.c:165-195`): 첫 호출에 1바이트 버퍼, 이후 청크마다 `strlen(buffer)` 로 현재 길이를 구해 `realloc`. 상한 없음.
`request_completed()` (`:389-398`) 가 남은 버퍼를 `free`.

실측 (`scratchpad/oos/body_measure.py`, 이 브랜치의 바이너리):

| 본문 | 결과 |
|---|---|
| 올바른 JSON | 200 |
| 올바른 JSON + NUL + `junk` | **200** — NUL 뒤는 조용히 버려짐 (`strlen` 이 NUL 에서 멈춤) |
| 닫는 `}` 앞에 NUL | 422 |
| 16 MiB (`pad` 필드) | 200, 0.14 s, 최대 RSS 약 46 MiB (11-07 재측정 47,320·47,724 KiB) |
| 64 MiB | **200, 4.43 s, 최대 RSS 약 155 MiB** (재측정 3.93·3.97 s, 158,268·159,388 KiB) — 16→64 MiB 에 시간 ×31.6 (재측정 ×28.1·×30.5, python; 선형 ×4, 제곱 ×16). 처음 적은 "47.8 MB"·"159.6 MB" 는 KiB 를 1000 으로 나눈 값이었다 |

정상 요청의 크기 (python): `text` 상한 10,000바이트를 전부 `\u00XX` 로 써도 60,000바이트. 나머지 필드는 수백 바이트.

| # | 결정 (권장안) | 근거 |
|---|---|---|
| A1 | `MAX_REQUEST_BODY` = 1 MiB (1,048,576) — 넘으면 **413** `PAYLOAD_TOO_LARGE` (`MHD_HTTP_CONTENT_TOO_LARGE`, 0.9.75 에서 권장 이름) | 정상 최대치의 약 17배 (공백·추가 필드 여유), 메모리 상한 |
| A2 | `Content-Length` 가 상한을 넘으면 **첫 호출 (헤더만 받은 때) 에 바로 413** — MHD 는 나머지를 버리고 연결을 닫으며 `100 Continue` 도 보내지 않는다. 길이를 미리 모르면 (chunked) 받은 바이트를 세다가 넘는 순간부터 버리고 (`*upload_data_size = 0` 으로 끝까지 소비) 업로드가 끝나면 413 — 이때는 keep-alive 유지 | MHD 는 첫 호출에서만 응답을 받고, 업로드 데이터 호출에서는 받지 않는다 (`microhttpd.h:2398-2411`, codex 지적으로 처음 서술 정정) |
| A2a | 한계: 연결 타임아웃 120 s 는 **무활동** 기준 (`microhttpd.h:1579`) 이라, 조금씩 계속 보내는 클라이언트의 연결 점유는 막지 못한다 (메모리만 막음) — 문서에 적고 앞단 프록시 권장은 유지 | |
| A3 | 상태 구조체 (`data`, `len`, `cap`, `too_large`, `has_nul`) — 청크마다 `strlen` 없음, 용량은 2배씩 (상한 + 1 까지), 덧셈은 넘침 검사. `request_completed()` 와 모든 실패 경로가 `data` 와 구조체를 함께 해제 | 측정된 제곱 비용. 정확한 크기의 `realloc` 만으로는 선형이 보장되지 않는다 (codex 지적) |
| A4 | 본문 어디에든 **원시 NUL 바이트** 가 있으면 **422** (`VALIDATION_ERROR`). 상한 초과와 NUL 이 함께면 413 이 우선 | JSON 에 원시 NUL 은 올 수 없다. 지금은 NUL 뒤를 버리고 200. "루트 값 뒤의 바이트는 무시" 라는 문서상 예외를 원시 NUL 에 한해 좁히는 것이므로 문서에 적는다 (`test_nul_guard.py:137-153` 의 뒤따르는 `\u0000` 은 원시 NUL 이 아니라 그대로 200) |
| A5 | 다른 동작은 그대로: 루트 값 뒤의 (NUL 아닌) 바이트는 지금처럼 cJSON 이 무시 (`test_nul_guard.py` 가 그렇게 기대), 오류 본문 형식 동일 | 범위 한정 |

## B. normal 백엔드의 이스케이프 이중 해제

현재: cJSON 이 이미 디코드한 번역문에 `unescape_string()` (`utils.c:791`, `backend_normal.c:659`) 을 한 번 더 적용 — `\n` `\t` `\r` `\\` `\"` `\'`
여섯 가지를 바꾼다. 첫 커밋 (`a26797c`, 2025-10-10) 부터 있었고 이유 기록은 없다. 그래서 번역문에 백슬래시가 그대로 있으면 바뀐다:
`C:\new\table` → 줄바꿈·탭, `\\` → `\`, 정규식·LaTeX·Windows 경로 손상. 반대로 줄바꿈을 `\n` 두 글자로 내는 모델에는 이 처리가 필요했을 수 있다.
codex 백엔드는 이 처리를 하지 않는다 (지금도).
배포: dkfs·pge4 는 codex, `transbasket.local.conf` 만 normal (TranslateGemma, Ollama) — 그 모델의 실제 출력은 이번에 확인할 수 없다 (11-03 실측 503).

| # | 선택지 | |
|---|---|---|
| B1 (권장, **결정**) | 설정 키 `NORMAL_UNESCAPE` 추가, **기본 `yes` (지금과 같음)**, `no` 면 cJSON 이 디코드한 그대로 (이모지 제거·공백 정리는 그대로 — 그래서 `\t`·`\r` 은 어느 쪽이든 결국 공백 하나, 줄바꿈만 남음, `utils.c:760-783`). 비스트리밍·SSE 둘 다 같은 지점 (`backend_normal.c:659`) 을 지난다. 캐시 키는 원문만으로 만들어지므로 그대로이고, **이미 캐시된 번역은 옛 처리 결과를 계속 돌려준다** → `no` 로 바꿀 때 normal 캐시를 비울지 운영자가 정하도록 문서에 적는다 (잃은 백슬래시는 되살릴 수 없음) | 기존 배포 동작 무변경 |
| B2 | 기본 `no` (이중 해제 중단) + 키로 되살림 | 옳은 기본값이지만 `\n` 을 내는 모델이 있으면 출력이 바뀜 |
| B3 | 그대로 두고 문서만 | |

## C. codex 슬롯 대기가 선착순이 아님

현재 (`backend_codex.c:240-252`): `sem_trywait` 를 20 ms 마다 다시 시도 (`translation_pause(control, 20)`). 대기 마감 = `CODEX_TIMEOUT`
(그리고 전체 마감·취소). 슬롯이 풀리면 그 순간 시도한 요청이 가져가므로, 먼저 기다린 요청이 마감에 걸리고 나중 요청이 성공할 수 있다
(11-06 실측: parity 의 슬롯 시나리오에서 늦게 온 요청이 먼저 슬롯을 얻음).

| # | 결정 (권장안) | 근거 |
|---|---|---|
| C1 | 새 모듈 `slot_gate.[ch]`. 규칙: 대기열 넣기·판정·빼기·슬롯 증감·알림은 모두 한 뮤텍스 안. 획득 조건은 `head == 나 && 남은 슬롯 > 0` 하나 — 대기열이 비어 있지 않으면 새로 온 요청도 줄을 선다 (끼어들기 없음). 깰 때마다 조건을 다시 보고, 마감·취소로 떠나면 머리·가운데·꼬리 어디서든 빠진 뒤 broadcast. 획득 뒤에도 슬롯이 남으면 broadcast, 반납 때 broadcast. 조건 변수는 `CLOCK_MONOTONIC` (`pthread_condattr_setclock` 실패면 초기화 실패). FIFO 는 **대기열에 넣은 순서** (HTTP 도착 순서가 아님) | 정확성 규칙 (codex 지적으로 구체화) |
| C2 | 지금과 같게 두는 것: 먼저 획득을 시도한 뒤 자기 마감을 본다 (마감 직전 획득 허용), 단 대기 중에 전체 마감·취소가 지나면 다시 시도하지 않는다 (`translation_pause()` 가 false — 구현 검토에서 codex 지적으로 맞춤), 대기는 두 마감을 넘기지 않는다, 대기 마감 `CODEX_TIMEOUT` (전체 마감·취소도 끝냄), 취소 감지는 **20 ms** 조각 (지금 루프와 같음), 실패는 503 `codex concurrency limit reached` (retryable; 전체 마감은 facade 가 504, 종료는 503 으로 바꿈), 실행은 그 뒤 새로 `CODEX_TIMEOUT`, NULL control 이면 취소 없음 | 동작 변화는 순서뿐. 취소는 서버 종료 플래그 (클라이언트 끊김 아님) |
| C3 | `OpenAITranslator` 의 `sem_t codex_sem` 을 `SlotGate codex_slots` 로: `http_client.c:54` 초기화, `backend_codex.c` 획득·반납·init 실패·free, V8 하네스 (`tests/test_codex_backend.c:71,135`). 파기는 지금처럼 서버 종료 순서의 끝 (`MHD_stop_daemon` → inflight 파기 → translator 파기, `http_server.c:644-681`) — 그 시점에 대기자·보유자 없음 | 사용처 전수 (grep) |

## D. `cache_tool` 이 DB 를 WAL 로 바꿈

현재: `cache_tool` 은 `options=NULL` → `journal_mode=WAL` 요청. 파일에 남는 것은 WAL 뿐이라 (11-07 실측), 서버를 WAL 이 아닌 모드로 쓰는 DB 를
`cache_tool` 이 WAL 로 바꾼다 (migrate 의 원본·대상, backfill dry-run 포함).

| # | 결정 (권장안) | 근거 |
|---|---|---|
| D1 | `SqliteCacheOptions` 끝에 `keep_journal_mode` 추가 (0 으로 초기화되므로 기존 초기화식은 그대로). 참이면 **journal 모드 대입만** 건너뛰고 현재 모드를 읽어 로그에 쓴다 (불일치 경고 없음), synchronous·cache·mmap 은 그대로. `cache_tool` 의 SQLite 열기 **세 곳** (migrate 원본 `:774`·대상 `:782`, backfill `:939`, dry-run 포함) 에서 참. 나머지 명령은 text 전용 (`:1043`) | 도구는 서버 설정을 모른다 |
| D2 | `cache_tool` 이 **새로** 만드는 DB (migrate 대상, backfill dry-run 으로 생긴 파일) 는 SQLite 기본 `delete` 로 시작, 서버가 처음 열 때 설정값 (기본 WAL) 으로. 파일에 남는 것은 WAL 뿐이라 TRUNCATE 등은 새 연결에서 원래 `delete` 로 보인다 | 서버 동작은 그대로 |
| D4 | 같은 SQLite 파일끼리의 migrate 는 거부 — 둘 다 있으면 장치·inode 비교 (심볼릭·하드 링크·상대 경로), 둘 다 없으면 부모 디렉터리 realpath + 이름 (처음 구현은 realpath 만 비교해 하드 링크·아직 없는 경로를 놓침, 구현 검토). 그리고 두 DB 를 연 뒤 SQLite 가 실제로 연 파일 (`sqlite3_db_filename`) 로 한 번 더 — SQLite 는 `없는디렉터리/..` 를 글자로 접어 같은 파일을 연다 (valgrind 확인 중 내가 발견, 실측). WAL 이 아닌 DB 에서는 원본 SELECT 를 연 채 대상에 쓰므로 스스로 막힐 수 있다 (codex 추론, 지금은 도구가 WAL 로 바꿔서 드러나지 않음). 서버가 도는 중에 WAL 이 아닌 DB 에 `cache_tool` 을 쓰면 잠금이 길어질 수 있다는 점은 문서에 | 이 변경으로 새로 생길 수 있는 막힘 |
| D3 | 서버·시험 하네스의 NULL 옵션은 지금처럼 WAL/NORMAL | |

## 사용자 결정 (2026-11-07)

"권장안으로 결정합니다." → A1 1 MiB·413, A2 버리고 413, A3 길이 추적, A4 원시 NUL 422, B1 `NORMAL_UNESCAPE` 기본 `yes`,
C1-C3 도착 순 대기열, D1-D3 `cache_tool` 은 journal 모드를 바꾸지 않음.

실측 추가 (현재 바이너리): `Transfer-Encoding: chunked` 업로드 (Content-Length 없음) 200 → 상한은 받은 길이로 검사해야 한다.
`Expect: 100-continue` 에는 MHD 가 첫 호출에서 `100 Continue` 를 자동으로 보냄.

## 검증

| # | 대상 | 방법 |
|---|---|---|
| V1 | A | 신규 `tests/test_request_body.py`: 상한 −1/정확히/+1 바이트 (python 계산) 를 Content-Length 와 chunked 각각, `Expect: 100-continue` + 큰 Content-Length 는 본문 없이 바로 413 (100 없음), 64 MiB 는 413 이고 **기준 대비 RSS 증가** 가 작음 (동시 연결 여러 개 포함), 누적 시간은 **상한 아래** 크기들 (64 KiB~1 MiB) 에서 선형, NUL 위치별 (루트 뒤가 수정 전과 구별되는 경우) 422, 상한 넘는 청크 안의 NUL 은 413, 413·422 에서 upstream 호출·캐시 기록 0, 청크 1바이트~64 KiB 로 같은 결과, chunked 413 뒤 같은 연결의 다음 요청 정상, 중도 끊김·종료 때 누수 없음 (ASan) |
| V2 | B | `NORMAL_UNESCAPE` 기본·`yes`·`no` 각각: 여섯 이스케이프와 바뀌지 않는 것 (`\u`, `\/`, 모르는 이스케이프, 끝의 백슬래시) 의 응답 바이트 (python 기대값), 공백만 남는 결과, SSE 이벤트 경계에 걸친 이스케이프, 캐시에 저장되는 값, 미리 캐시된 옛 항목이 설정을 바꿔 재시작해도 그대로 나오는 것, 잘못된 값은 경고 후 기본. 기존 `test_normal_output.py` 의 기대값 (해제 적용) 은 기본값에서 그대로 통과 |
| V3 | C | 신규 C 단위 시험 (`slot_gate`, 시험 훅으로 대기열 넣기 순서 관찰): 획득 순서 = 넣은 순서 (슬롯 1개·여러 개), 머리·가운데·꼬리·전부 이탈 후 순서·슬롯 수 유지, 끼어들기 없음, 강제 깨움, 획득과 마감·취소가 겹치는 경우, 초기화 실패, 수천 회 반복, TSan·ASan. 서버: 넣은 순서를 로그로 확인한 뒤 느린 요청 4개·슬롯 1 의 상태가 그 순서로 [504, 504, 503, 503] (수정 전 바이너리는 순서가 바뀌는 경우가 나옴). parity 는 이 시나리오를 순서 없이 비교하므로 FIFO 증명에 쓰지 않는다 |
| V4 | D | SQLite 열기 세 곳 각각 (migrate 원본·대상, backfill 실제·dry-run): DELETE DB 는 `delete` 그대로, WAL DB 는 `wal` 그대로, 새로 생긴 DB 는 `delete` 이고 서버가 열면 `wal`, 명령 결과·데이터도 맞음, 같은 파일 migrate 는 거부 (경로 별칭 포함), 서버가 도는 중의 backfill 이 끝남 |
| V5 | 회귀 | `make test-*` 전부 (목록은 Makefile 에서), `make test-backend-parity PARITY_OLD_BIN=<1348a53 빌드>`, 프롬프트 회귀, ASan·TSan (각각 따로 빌드)·valgrind, 배포 설정 3개. 의도된 차이 (413, NUL 422, FIFO 순서) 는 parity 시나리오 밖 |

## codex 계획 검토 판정 (코딩 전)

| codex 주장 | 내 검증 방법 | 결과 |
|---|---|---|
| MHD 는 첫 호출에서 응답 가능 (나머지 버리고 연결 닫음), 업로드 데이터 호출에서는 불가 | `microhttpd.h:2398-2411` 열람 | ✅ 채택 (A2 정정 — 내 오류) |
| 120 s 는 무활동 타임아웃 | `microhttpd.h:1579` 열람 | ✅ 채택 (A2a) |
| chunked 는 길이 없이 누적 검사, keep-alive 는 다 받은 413 에서 유지 | 실측 (chunked 200) + 설계 | ✅ 채택 |
| 정확한 크기 `realloc` 만으로 선형 보장 안 됨 | 설계 검토 | ✅ 채택 (A3 2배 용량) |
| A4 는 `test_nul_guard` 와 충돌 없음 (그쪽은 `\u0000` 글자), 문서상 예외를 좁힘, 413 우선 정해야 | `test_nul_guard.py:137-153` 열람 | ✅ 채택 |
| B: 여섯 가지 한 번만, SSE·비스트리밍 공통, 캐시 키 불변, 기존 캐시 항목은 옛 결과 유지, `\t`·`\r` 은 공백으로 정리됨 | `utils.c:760-843`, `trans_cache.c:35-40` 열람 | ✅ 채택 (B1 문구·문서) |
| C: 정확성 규칙 (한 뮤텍스, 머리만 획득, 끼어들기 금지, broadcast, 단조 시계), 넣은 순서 = FIFO | 설계 검토, `inflight_translation.c` 의 단조 조건 변수 선례 | ✅ 채택 (C1) |
| C: 먼저 시도 후 마감, 20 ms 조각 (계획의 50 ms 는 지연 증가) | `backend_codex.c:241-247` 열람 | ✅ 채택 (C2 — 내 오류) |
| C: `codex_sem` 사용처에 `http_client.c:54`·init 실패·free 포함, 파기 순서 | grep, `http_server.c:644-681` | ✅ 채택 (C3) |
| D: SQLite 열기는 세 곳뿐, stats 등은 text 전용 | `cache_tool.c:774,782,939,1043` 열람 | ✅ 채택 (D1, V4 정정 — 내 오류) |
| D: 대입만 건너뛰고 읽기는 유지, 필드는 끝에 추가 | 설계 검토 | ✅ 채택 |
| D: TRUNCATE 는 다시 열면 `delete` (보존 불가) | 11-07 실측과 같음 | ✅ 정정 (내 오류) |
| D: 같은 파일 migrate 가 WAL 아닌 DB 에서 스스로 막힐 수 있음 (추론) | 코드 경로 (원본 SELECT 연 채 대상 쓰기) 열람, 재현은 구현 뒤 V4 에서 | ⚖️ 채택 (D4 거부로 막음) |
| V1-V5 보강 | 계획 대조 | ✅ 채택 |

## codex 구현 검토 판정 (코딩 뒤, 2026-11-07)

먼저 내 오류: V1 의 "상한 바로 아래 본문을 512바이트씩 — 수정 전 4.98 s" 는 **시험 버그**였다. 정보 줄이 `o.stop()` (종료 때 그룹 정리 약 5 s)
뒤에 시간을 재고 있었다 (단계별 시간과 서버 로그로 확인: 업로드 0.01 s, `HTTP server stopped` 17:55:03 → `drained` 17:55:08).
수정 전 바이너리도 1 MiB 이하는 0.01 s 안팎이다. 그래서 상한 아래의 시간 측정으로는 옛/새를 구별할 수 없고, 제곱 비용을 막는 것은 상한이다
(64 MiB 재측정 3.93 s·3.97 s, 16→64 MiB 시간 ×28.1·×30.5 — python). README 문구를 고쳤다.

| codex 주장 | 내 검증 방법 | 결과 |
|---|---|---|
| 같은 파일 판정이 아직 없는 경로·하드 링크를 놓친다 | 실측 (작업 중 바이너리): 없는 같은 경로 rc 0·파일 생성·0건, DELETE DB 의 하드 링크 rc 1 `Failed: 3 entries` (원본 3행 그대로) | ✅ 채택 — `same_file()` (inode / 부모 realpath). V4 에 같은 경로·심볼릭·하드 링크·상대 경로·없는 경로 + 서로 다른 새 파일 둘은 허용. 변이 2개 (예전 realpath 판정, 새 경로 검사 없음) 모두 검출 |
| 깨어난 뒤 control 보다 획득을 먼저 본다 — 예전엔 `translation_pause()` 가 false 면 다시 시도하지 않았다, 대기 시각에 전체 마감이 빠졌다 | `git show HEAD:src/backend_codex.c` 의 옛 루프, `include/translation_control.h:36-50` (마지막 조각을 남은 시간으로 자름) 열람 | ✅ 채택 — 대기 뒤 control 확인, 대기 시각 = min(20 ms, 자기 마감, 전체 마감). 단위 시험 3개 추가 (전체 마감 뒤 풀린 슬롯을 받지 않음 / 전체 마감에 맞춰 끝남 / 자기 마감 직후 풀린 슬롯은 받음 — 예전 순서). 변이 3개 검출 |
| V1: 거부 요청이 이미 캐시된 원문을 써서 "upstream 호출 없음" 이 증명이 안 된다, `junk` 검사는 무의미 | `tests/test_request_body.py` 열람: `body_of()` 가 늘 `가나다`, `junk` 는 루트 값 밖 | ✅ 채택 — 거부 요청마다 처음 보내는 원문, 종료 때 저장된 캐시에 그 원문이 없음을 JSON 으로 확인 (text 캐시는 종료 때 저장됨 — 처음엔 실행 중 파일을 비교하려다 파일이 없어 알게 됨) |
| V1: 64 MiB 응답 상태를 버린다 | 같은 파일 열람 | ✅ 채택 — 5건 모두 413 확인 |
| V1: 한 번의 `< 2 s` 는 선형 증명이 아니다, 소켓 쓰기 크기가 MHD 콜백 크기를 정하지 않는다 | 실측: 64 KiB·256 KiB·1 MiB 를 옛/새 바이너리로 — 둘 다 0.003~0.05 s | ⚖️ 부분 — 위 내 오류와 같은 결론. 검사는 "2 s 안에 응답" 이라는 정상성 한계로만 두고 설명을 고쳤다. 선형성은 코드 (길이 기록·2배 용량) 로 보장 |
| 단위 시험의 join 확인이 다른 대기자의 이탈을 join 으로 볼 수 있다 | `tests/test_slot_gate.c` `start_in_order()` 열람 | ✅ 채택 — 누적 join 수 (`slot_gate_joins()`) 로 확인하고 매번 CHECK. 이 결함은 거짓 통과가 아니라 거짓 실패 쪽 |
| hand-off ≤ 10 ms 는 스케줄러에 민감 | 반복 실행: plain 5회 최악 5 ms, TSan 3회 최악 1 ms. 변이는 12~40 ms | ⚖️ 기준 유지 |
| signal 변이는 "사실상 동등" 이 아니다 — non-head 를 깨울 수 있다 | 변이 반복: 20회 기준 8번 중 4번만 검출 (3슬롯 동시 반납, 회당 약 3%) | ✅ 채택 (내 서술 오류) — 그 경우를 200회로 (보유 30 ms 유지; 보유를 없애면 0/10). 10번 중 10번 검출 |
| V3 서버 시험의 "Starting" 로그는 대기열 넣기 전, 옛 바이너리도 우연히 통과할 수 있다 | `src/backend_codex.c:237` (로그) vs `:244` (획득) | ✅ 채택 — 설명을 고침 (0.3 s 간격이라 순서는 같고, 정확한 join 순서는 단위 시험이 증명) |
| V4: 행 수만 본다 (hint·dry-run 무변경 미확인), 대상이 늘 새 파일, 서버 기동 확인 없음 | `tests/test_cache_tool_journal.py` 열람 | ✅ 채택 — hint 3건 / dry-run 행 그대로, 기존 DELETE·WAL 대상의 모드 유지, 기동 확인. 내 기대값 오류 둘을 실측으로 고침: dry-run 도 파일 바이트는 바뀜 (매 기동 trigger·view 재생성), 기존 대상 행은 가짜 hash 라 1 + 3 = 4행 (`select` 로 확인) |
| (내 발견, valgrind 확인 중) `./없는디렉터리/../a.db` 는 열기 전 판정을 통과 | 실측: rc 1, `Failed: 3 entries` (원본 3행 그대로) — SQLite 가 `..` 을 글자로 접음 | 고침 — 연 뒤 `sqlite3_db_filename()` 두 파일의 inode 비교. V4 에 추가, 이 검사를 뺀 변이 검출 |
| A·B·C·D 의 나머지 "결함 없음" | B: `Config` 생성 전수 grep — `config_loader.c:440` 한 곳. `con_cls` 는 `/translate` 만 설정 (`http_server.c` `request_handler()` 열람) | ⏭️ 그 밖 (MHD 의 Content-Length 문법 검사 등) 은 문서에 쓰지 않아 미검증 |
