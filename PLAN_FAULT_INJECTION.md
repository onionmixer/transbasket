# 실패 주입 시험 계획

작성 2026-11-07. 작업 위치: worktree `../transbasket-fault`, 브랜치 `test/fault-injection` (main `61cc36f`).
요청: "실패 주입 시험을 진행합니다" — 남은 작업 7번 ("코드 검토로만 확인한 경로"). 원칙: 계산·기대값은 python,
codex 는 코딩 전 계획 검토 입력일 뿐, 별도 worktree, 모든 기준 통과 후 main fast-forward. `파일:줄` 은 `61cc36f` 기준.

## 1. 목록을 만든 방법

기록 (HANDOFF·PLAN_BACKEND_MODULES·PLAN_SOURCE_HINT_METADATA) 에만 기대지 않고, 커버리지로 지금 시험이 지나지 않는
분기를 실측했다: `-O0 --coverage` 빌드로 `make test-*` 31종 (parity 제외) 실행 → gcda 143개 → `gcov-12 --json-format` 을
python 으로 줄·분기별 합산 (`scratchpad/fault/cov_*.py`).

한계 (해석에 반영):
- **줄 커버리지는 부족하다**: `if (!wrapped) { ...; goto cleanup; }` 처럼 조건과 실패 처리가 한 줄이면 조건만 평가돼도
  "실행" 이다. 그래서 분기 커버리지로 판정했다.
- **과소 계상**: SIGKILL 로 끝난 프로세스는 gcda 를 쓰지 못한다 (`daemon.c` 30.7% 가 그 예). "미실행" 후보는 시험 코드를
  grep 해 다시 확인했다.
- 같은 소스를 매크로와 함께 `#include` 하는 하네스 (test_sse) 는 분기 배치가 달라 따로 집계된다.
- TSan 하네스는 커버리지 계수기 경합으로 실패한다 (`__gcov0.slot_gate_release`) — 계측 인공물, 평소 TSan 실행은 통과.

## 2. 대상 (커버리지와 시험 코드로 미시험 확인)

| # | 경로 | 근거 |
|---|---|---|
| F1 | codex 임시 cwd 생성 실패 (`backend_codex.c:254-258`): `-C`/`-o` 없이 실행, 결과는 stdout, structured → plain, 남는 파일 없음 | `:342` 의 "cwd 없음" 분기 0회, `CODEX_RUN_DIR` 을 못 쓰게 만드는 시험 0건 (grep) |
| F2 | codex 프롬프트 생성 실패 4곳: 지시문 (`:282`), 감싼 원문 (`:291`), core — `{TEXT}` 치환·덧붙이기 (`:300-302`), 최종 프롬프트 (`:316`) | 네 분기 모두 0회. 기존 `TB_TEST_FAIL_PROMPT` 는 그 앞 한 지점 |
| F3 | S1 (`codex_driver_exec.c:23-25`): 드라이버 상태 할당 실패 → `Error: Memory allocation failed` + 기동 실패 | 분기는 이미 실행됨 (`tests/test_codex_backend.c:256-259`) — 빠진 것은 **확인**: 로그 문구, 종료 코드, 바인드 안 함, `-d` |
| F4 | source_hint SQLite 저장의 SQL 실패: SAVEPOINT 실패 (`cache_backend_sqlite.c:881`), `ROLLBACK TO`/`RELEASE` 실패 (`:891-898`), 성공 쪽 `RELEASE hint` 결과 무시 (`:901`), 두 번째 시도도 실패 (`:867`), `ROLLBACK` 재시도·"stuck" 로그 (`:837-841`) | 모두 0회. dc72f4d 기록에서도 "코드 확인". 이미 시험되는 것: hint 의 `RAISE(ABORT)`/`RAISE(ROLLBACK)` (`tests/test_source_hint_store.c:241`), `SQLITE_FULL` (`:203`), **COMMIT 실패** (commit hook, `:330`) — 처음에 COMMIT 실패를 미시험으로 적은 것은 내가 `cache_backend_sqlite.c:904` 의 분기를 잘못 읽은 것 (`[1223, 0, 2, 1221]`: COMMIT 실패 2회, 0회는 마감 쪽) |
| F5 | 저장 경로의 할당 실패: SQLite 호출자 사본 (`cache_backend_sqlite.c:863`), text 호출자 사본·저장 사본 (`cache_backend_text.c:469`, `:475`), text 새 행 삽입 (`:338` entry, `:350-351` 문자열 둘, `:367` 용량 `realloc`) | 0회. 새 행·사라진 행·용량 경계 픽스처. 실패한 삽입도 `next_id` 를 하나 쓴다 (`:347`, 무해 — 그 필드는 단정하지 않음) |
| F6 | 백엔드 자체의 마감 재확인: text (`cache_backend_text.c:479`), SQLite COMMIT 직전 (`cache_backend_sqlite.c:904`) | 0회 — 감싸는 `trans_cache_store_result()` 가 잠금 직후 먼저 보므로 (`trans_cache.c:254`) 기존 마감 시험은 거기서 끝남 |

읽다가 본 것: text 저장 머리 주석 (`cache_backend_text.c:451-454`) 은 "control 을 다시 볼 필요 없다" 인데 본문은 다시 본다 — 주석이 옛 상태. 고친다.
설정 실패의 응답: `status_code 0`·retryable 아님 → HTTP 502 (`http_server.c:393-394`). `strdup(setup_error)` 까지 실패하면
메시지 NULL → `inflight_wait()` 가 `"Translation failed"` 로 바꿈 (`inflight_translation.c:116`) — 안전.

## 3. 범위 밖 (커버리지에 나왔지만 이번에 하지 않음, 목록만)

`slot_gate_init` 의 pthread 실패 (`slot_gate.c:15-27`)·그 로그 (`backend_codex.c:436`), structured 출력이 엄격 검증은 통과했는데
cJSON 이 못 읽는 경우 (`:174-176`, cJSON 내부 할당 실패로만 — 이 파일 매크로로는 못 만듦, cJSON 할당자 훅 필요), 구분자 8회 충돌 (`:92`), `trace_file.c` 실패 경로, text 캐시 적재의 깨진 줄
(`cache_backend_text.c:79-133`), `trans_cache.c` 의 SHA-256 실패, `cache_tool.c`·`config_loader.c`·`utils.c` 의 나머지,
`daemon.c` (SIGKILL 로 과소 계상, test-daemon 이 따로 덮음). 원하면 다음 묶음.

## 4. 방법

| # | 방법 | 대상 |
|---|---|---|
| M1 | **할당 실패 훑기**: 하네스가 대상 소스를 `#include` 하고 그 파일 안의 `malloc`·`calloc`·`realloc`·`strdup`·`strndup` 만 세는 래퍼로 바꾼다 (test_sse 방식: 시스템·프로젝트 헤더를 먼저 포함하고, 래퍼 정의 → 매크로 → 대상 소스 → `#undef`; 포함한 `.c` 는 링크 목록에서 뺀다). 다른 번역 단위·라이브러리 (cJSON, uuid) 의 할당은 세지 않는다. 가짜 드라이버의 할당은 매크로 밖 (지금 픽스처의 `dup_len()` 은 `malloc` 결과를 보지 않음, `tests/test_codex_backend.c:58`). 두 방식: **단일** (k번째만 실패, k = 1..N, N 은 성공 1회로 셈) 과 **지속** (k번째부터 전부 실패) — 실패 뒤에만 가는 할당 (예: 오류 문구 `strdup`, `backend_codex.c:359`) 은 지속 방식으로만 닿는다 | F2 (`backend_codex.c` + `prompt_builder.c`), F5, Q2 |
| M2 | **SQLite authorizer**: `sqlite3_set_authorizer()` 로 정해진 문장의 n번째 준비를 `SQLITE_DENY` → `sqlite3_exec` 가 `SQLITE_AUTH` 로 실패, 그 문장은 실행되지 않음 (python 3.37.2 실측, 문장 캐시 끔). 동작 코드: `SAVEPOINT`/`RELEASE`/`ROLLBACK TO` = `SQLITE_SAVEPOINT` + `"BEGIN"`/`"RELEASE"`/`"ROLLBACK"`, `BEGIN IMMEDIATE`/`COMMIT`/`ROLLBACK` = `SQLITE_TRANSACTION` + `"BEGIN"`/`"COMMIT"`/`"ROLLBACK"` (실측). `sqlite3_exec` 는 매번 준비하므로 매번 묻는다. 오류 쪽 `ROLLBACK TO`/`RELEASE` 는 먼저 hint 실패 (기존 ABORT 트리거) 가 있어야 닿는다 — 조합으로 | F4 |
| M2b | **SQLite 내부 할당 실패 훑기**: `sqlite3_config(SQLITE_CONFIG_MALLOC)` 는 첫 `sqlite3_open` 전 (암묵적 `sqlite3_initialize`) 에만 된다 — 하네스 시작 때 전달만 하는 할당자를 설치하고, 저장 한 번 동안만 켠다. 할당자 전체 인터페이스 (realloc 소유권, size, roundup) 를 지키고, 매 k 마다 같은 새 픽스처 (lookaside·page cache 재사용으로 수가 달라짐). 실패한 할당을 SQLite 가 다시 시도할 수 있어 "k 마다 NOMEM" 을 요구하지 않는다 | F4 보강 — 어떤 문장이 NOMEM 을 내도 아래 불변식 |
| M3 | 백엔드의 `ops->store_result` 를 마감이 지난 control, 그리고 취소된 control 로 **직접** 호출 (단일 스레드 픽스처) | F6 |
| M4 | 서버 + 가짜 codex (argv·cwd·stdin 기록): 기동 뒤 `CODEX_RUN_DIR` 을 0500 으로 (기동 때는 `access(W_OK)` 로 보고 경고만, `http_server.c:513-520`; root 면 무효라 일반 사용자로) | F1 |
| M5 | `TRANSBASKET_TEST_HOOKS` 서버 + `TB_TEST_FAIL_DRIVER_INIT`: 종료 코드·로그 줄·바인드 안 함, `-d` 면 실행한 쪽이 그 줄을 보여 주고 1 | F3 |

## 5. 불변식 (기대값)

- F2 (k 마다, plain·structured, auto·명시적 원문, `{TEXT}` 템플릿·옛 템플릿): NULL 반환, `error->status_code` 0 (→ HTTP 502)·retryable 아님,
  메시지는 설정 오류 문구 또는 (그 `strdup` 자체가 실패한 k) NULL, 드라이버 호출 0회, 슬롯 수 원래대로, 임시 cwd·스키마 파일 없음,
  ASan/LSan 누수 0. 실패가 k 번째 할당과 **무관한 곳** (예: trace 저장) 이면 정상 결과여야 한다 — k 마다 실제로 무엇이 실패했는지 기록.
  드라이버가 불렸다면 받은 프롬프트에 치환 안 된 자리표시자 (`{SOURCE_LANG}` `{TARGET_LANG}` `[TARGET LANGUAGE]` 등) 가 없어야 한다 —
  지금 코드는 명시적 원문에서 이것을 어긴다 (Q2).
- F4/F5/M2b (실패 종류마다, 부모 삽입·count 증가·번역 변경 × hint 없음·기존 hint 있음): 반환값과 **다른 연결에서 본 커밋 상태**가 일치
  — 0 이면 부모가 **정확히 한 번** 적용 (재시도해도 count 두 번 아님), hint 는 쓰였거나 (성공) 안 쓰였음 (hint 실패 — 이때 기존 hint 는 그대로),
  −1 이면 부모·hint 모두 그대로. hint 는 부모 없이 커밋되지 않음. 호출자 사본은 커밋 성공 때만 바뀜. 끝나면 연결이 트랜잭션 밖
  (`sqlite3_get_autocommit`) 이고 다음 저장이 정상 — "stuck" 은 예외 (Q1).
- F4 성공 쪽 `RELEASE hint` 실패 (authorizer 거부): 부모+hint 커밋·0 반환 (SAVEPOINT 가 열린 채의 COMMIT 은 전부 커밋 — python 실측).
  트랜잭션을 끝내는 종류의 RELEASE 오류라면 이어지는 COMMIT 이 실패해 −1 — 그 경우는 M2b 가 덮는다.
- F4 `ROLLBACK` 이 계속 실패: "stuck" 로그. **실측 (C, 실제 백엔드, 스크래치 하네스)**: 거부를 풀어도 이후 모든 저장이
  `cannot start a transaction within a transaction` 으로 −1, 다른 연결의 `BEGIN IMMEDIATE` 는 `database is locked` (쓰기 잠금 유지 —
  서버가 도는 동안 `cache_tool` 도 막힘), 커밋된 것은 그 전 행뿐 → 결정 Q1.
- F6 (마감 지남·취소 각각): −1, 아무것도 **커밋** 안 됨 (SQLite 는 부모·hint 문장을 실행한 뒤 COMMIT 직전에 보고 ROLLBACK — 다른 연결로 확인),
  호출자 사본 그대로, 연결은 트랜잭션 밖, 누수 0.
- F1 (가짜 codex 가 정상 출력, 마감 여유): 200, 가짜 codex argv 에 `-C`·`-o`·`--output-schema` 없음, cwd 는 서버 cwd, 결과는 stdout
  (trim·코드펜스 처리 뒤), stdin 의 프롬프트는 plain 문구, `SOURCE_HINT_MODE=model` 이어도 hint 는 script 분류기 값 (`http_server.c:400-404`),
  `CODEX_RUN_DIR` 에 아무것도 안 남음, 스키마 파일 시도 없음. 권한을 되돌리면 다음 요청은 다시 `-C`·`-o`.
  (처음에 적은 "로그 (DEBUG) 에 mkdtemp 실패 줄" 은 틀림 — 그 줄은 `LOG_DEBUG` 라 `-DDEBUG` 빌드에서만 컴파일되고 (`include/utils.h:11-15`),
  일반 빌드는 이 대체 동작을 **로그에 남기지 않는다**. 시험은 그 사실을 확인한다.)
- F3: 종료 코드 1, 로그 `Error: Memory allocation failed`, 포트에 바인드하지 않음. `-d` 면 실행한 쪽이 같은 줄을 보여 주고 1.

## 6. 결정이 필요할 수 있는 것

| # | 내용 | 권장 |
|---|---|---|
| Q1 | "stuck" 뒤 회복 (실측으로 확인된 결함): `store_result` 시작 때 연결이 트랜잭션 안이면 먼저 `rollback_all()` 하고 **그 저장은 −1 로 건너뜀** (구현 검토에서 보강 — 9절) | 고침 (작고 국소적) |
| Q2 | 명시적 원문 언어의 지시문 생성이 치환 실패 (`str_replace()` NULL = 할당 실패일 때만, `prompt_builder.c:42-69`) 를 무시하고 진행 (`:211-269`) — 메모리 부족 때 `{TEXT}`·`[TARGET LANGUAGE]` 가 남은 프롬프트로 번역해 200·캐시될 수 있음. auto 경로 (`:121-131`) 처럼 NULL 을 돌려주면 normal 은 502·캐시 없음 (`backend_normal.c:365-371`), codex 는 설정 실패 502 (`backend_codex.c:282`) | 고침 — 성공 때 프롬프트는 바이트 단위로 같음 (프롬프트 회귀로 확인) |

사용자 결정 (2026-11-07): Q1 "회복하게 고침", Q2 "실패로 처리하게 고침" (둘 다 권장안).

## 7. 검증

새 시험마다 변이로 잡히는지 확인 (예: cleanup 의 슬롯 반납 제거, `rmdir` 제거, `ROLLBACK TO` 결과 무시, text 마감 재확인 제거,
caller 사본을 커밋 전에 반영). 전체 회귀 (`make test-*`, parity, 프롬프트 회귀, ASan 서버), valgrind. 운영 바이너리에 주입 지점
문자열 0건 (`TB_TEST_` grep). 의도된 코드 변경은 주석 정정과 (결정되면) Q1·Q2 뿐.

## 8. codex 계획 검토 판정 (코딩 전)

| codex 주장 | 내 검증 방법 | 결과 |
|---|---|---|
| F3 분기는 이미 실행됨 (`tests/test_codex_backend.c:256`), 빠진 것은 확인 | 그 줄 열람 | ✅ 채택 (표 문구) |
| F4: COMMIT 실패는 commit hook 으로 이미 시험 (`tests/test_source_hint_store.c:330`), ABORT/ROLLBACK·FULL 도 | 그 함수 열람 (`tests/test_source_hint_store.c:330-350`), 커버리지 `cache_backend_sqlite.c:904` 분기 `[1223, 0, 2, 1221]` | ✅ 채택 — **내 오류** (분기를 잘못 읽음) |
| F2: 명시적 원문의 치환 실패를 무시하고 진행 → 자리표시자가 남은 프롬프트 | `prompt_builder.c:42-69`, `:121-131`, `:208-269` 열람 | ✅ 채택 — 실제 결함 (Q2) |
| `backend_codex.c:174-176` 이 "사실상 불가" 는 틀림 — cJSON 할당 실패로 가능 | 그 줄 열람 (`cJSON_ParseWithOpts` NULL) | ✅ 채택 (범위 밖 문구) |
| M1: 헤더 먼저, 포함한 `.c` 는 링크에서 빼기, cJSON 은 못 셈, 가짜 드라이버 `dup_len()` 은 결과를 안 봄, 실패 뒤에만 가는 할당은 단일 방식으로 못 닿음 | `tests/test_codex_backend.c:57-60`, `Makefile:199`, `backend_codex.c:359` 열람 | ✅ 채택 (지속 방식 추가) |
| M2 동작 코드·인자, 거부 = 실행 안 됨, `sqlite3_exec` 는 매번 묻는다 | python 실측 (문장 캐시를 끄지 않은 첫 측정은 캐시 때문에 무효였음 — 끄고 다시) | ✅ 사실 |
| M2b: 설치는 첫 open 전, 저장 동안만 켜기, 재시도 때문에 k 마다 NOMEM 아님 | 문서 지식 + 구현 때 `sqlite3_config` 반환값으로 확인할 것 | ⚖️ 채택 (구현에서 반환값 단정) |
| M4: 기동은 `access(W_OK)` 로 보고 경고만, root 면 무효 | `http_server.c:513-520` 열람 | ✅ 채택 |
| M5: 기동 실패는 바인드 전 `return 1` (`main.c:205-210`), `-d` 는 실행한 쪽이 1 | `main.c:200-216`, `daemon.c:128-140,205-214` 열람 | ✅ 사실 |
| Q1 추론 (stuck 뒤 회복 안 됨, 잠금 유지) 이 맞다 | C 실측 (위 5절) | ✅ 사실 → Q1 |
| 불변식 보강: 다른 연결로 본 커밋 상태, 부모 정확히 한 번, 기존 hint 유지, 취소도, F1 은 plain 프롬프트·script hint 까지 | `cache_backend_sqlite.c:788-810`, `http_server.c:395-404` 열람 | ✅ 채택 (5절) |
| F5 에 text 삽입 할당 추가, 실패한 삽입이 `next_id` 소모 | `cache_backend_text.c:333-381` 열람 | ✅ 채택 |
| 성공한 rollback 정리는 `test_request_coalescing.py:266` 이 이미 덮음 | 그 줄 열람 (캐시 쓰기 실패 시험) | ⏭️ 행동 변화 없음 |
| 마감 뒤 공개 때 응답이 덮일 수 있음 (`inflight_translation.c:131`) | 그 줄 열람 | ⏭️ F2 시험은 백엔드 수준이라 무관 |

## 9. 구현 결과 (2026-11-07)

새 시험 3개 (`make test-fault-codex`, `test-fault-store`, `test-fault-server`). 운영 코드 변경은 Q1·Q2 와 주석 1곳.

| 시험 | 내용 | 수정 전 | 수정 후 |
|---|---|---|---|
| `test-fault-codex` | `prompt_builder.c`+`backend_codex.c` 의 할당을 단일·지속 방식으로 전부 실패 (템플릿 2 × 명시적·auto structured·auto plain), `build_instruction_message()` 단독 (normal 이 부르는 형태) | **38건 실패** — 명시적 원문에서 `[TARGET LANGUAGE]`·`{{LANGUAGE_TO}}`·`{SOURCE_LANG}`… 이 남은 프롬프트가 드라이버까지 감, `{TEXT}` 템플릿은 원문 없이 `{TEXT}` 글자가 남은 지시문 | 전부 통과: 실패는 항상 드라이버 호출 전 (status 0 → 502), 슬롯·임시 cwd 정리, ASan 누수 0. 프롬프트 회귀 24건 수정 전과 같음 |
| `test-fault-store` | authorizer 시나리오 9개 × 고정물 7~11 (사라진 행 포함), 낡은 사본 시나리오, 백엔드 직접 호출 (마감 지남·취소) 44회, 할당 실패 훑기: SQLite 자체 할당 932회 (저장 한 번에 최대 60), sqlite 백엔드 C 할당 6회, text 백엔드 C 할당 58회 (용량 경계 포함) | 첫 판 (사라진 행 없음): **345건 실패** — "stuck" 뒤 회복 안 됨 (authorizer 8 고정물 + SQLite 메모리 부족 107회): 다음 저장 −1, 다른 연결 `database is locked` | 전부 통과 (최종 판: stuck 149회 모두 회복). 반환값 = 다른 연결로 본 커밋 상태, 부모 정확히 한 번, 기존 hint 유지, 부모 없는 hint 0, 호출자 사본 (번역·count·last_used·hint) |
| `test-fault-server` | F1 (실행 디렉터리 0500) · F3 (S1) 실제 서버 | 통과 (결함 없음) | 통과 |

알게 된 것:
- `cache_backend_sqlite.c:867` 의 "두 시도 모두 `continue`" 는 **도달 불가** — `continue` 하는 두 경로 (`:887-888`, `:896-897`) 가 모두 `want_hint = NULL` 로 만든 뒤라 두 번째 시도는 `continue` 하지 않는다.
- 성공 쪽 `RELEASE hint` 결과를 보지 않는 것 (`:901`) 은 무해 — 거부돼도 COMMIT 이 SAVEPOINT 까지 커밋 (5 고정물 실측).
- 오류 쪽 `ROLLBACK TO` 실패도 ABORT 류 hint 실패 뒤라면 결과가 같다 (문장 단위로 이미 되돌려짐). 그 검사를 없애는 변이는
  9 시나리오 가운데 "재시도의 BEGIN 도 거부" 에서만 드러났다 (변이를 다시 돌려 시나리오별 결과 확인).
- Q1 이 다른 작업을 버리지 않는 근거: 같은 연결에서 명시적 트랜잭션을 여는 곳은 기동 때 스키마 이전 (`cache_backend_sqlite.c:219`) 뿐이고,
  `store_result` 를 부르는 곳은 서버 (`http_server.c:406`, 캐시 쓰기 잠금 안) 와 `cache_tool migrate` 의 `trans_cache_import_entry()`
  (`trans_cache.c:212`, 원본과 다른 연결 — 같은 파일은 거부) 뿐이다 (전수 grep). 그러므로 시작 때 열린 트랜잭션은 실패로 남은 것뿐이다.
- 커버리지 수치의 한계 그대로: SIGKILL 로 끝난 서버의 실행은 세지 않았다.

변이 (`scratchpad/fault/mutants.py`, 소스를 바꾸고 해당 시험만 실행, 원본 바이트 복원 확인) 20개 — 19개를 새 시험이, 1개를 기존 시험이 잡음:

| 변이 | 잡은 시험 |
|---|---|
| 정리 때 슬롯 반납 없음 / 임시 cwd 삭제 없음 / 실패 때 core 누수 (ASan) | `test-fault-codex` |
| Q2 되돌림 (옛 템플릿) / `{TEXT}` 실패 무시 | `test-fault-codex` |
| Q1 되돌림 / 회복하는 저장을 건너뛰지 않음 / 다시 넣은 행의 사본에 지워진 hint 가 남음 / `rollback_all()` 재시도 없음 / `ROLLBACK TO`·`RELEASE` 실패 무시 / COMMIT 직전 마감 검사 없음 / 커밋 전에 호출자 사본 반영 / text 마감 재확인 없음 / text 새 행 문자열 실패 때 누수 (ASan) / text 저장 사본 실패 때 누수 (ASan) | `test-fault-store` |
| mkdtemp 실패 때 대체 없이 실패 / cwd 없이 structured 시도 (처음엔 놓침 — 응답이 같아서. "스키마 시도 없음" 검사를 넣어 잡음) / S1 문구 변경 | `test-fault-server` |
| hint 실패가 트랜잭션을 끝냈을 때 부모 재시도 없음 | 새 시험은 놓침 — 할당 훑기는 "−1 + 아무것도 안 바뀜" 을 맞는 결과로 받으므로 재시도 누락을 구별하지 못한다. 기존 `test-source-hint-store` 의 `RAISE(ROLLBACK)` (부모가 반드시 커밋돼야 함) 가 잡음 |

### codex 구현 검토 판정 (2026-11-07)

먼저 내 오류 둘: (1) 첫 Q1 수정은 낡은 사본 위에 쓸 수 있었다 (아래 첫 행). (2) text 저장 주석을 고치며 "모든 할당이 그 검사 전" 이라고
새로 틀리게 썼다 — 새 행의 할당은 그 뒤 `text_backend_add()` 에서 한다. 또 과정상 실수: 회귀가 이 worktree 에서 도는 중에 변이를 돌려
소스를 바꿨다 — 그 회귀는 멈추고 처음부터 다시 돌렸다.

| codex 주장 | 내 검증 방법 | 결과 |
|---|---|---|
| Q1 회복이 방금 되돌린 트랜잭션의 사본 위에 쓴다: 커밋 A×1, stuck 안의 B×1 을 같은 연결 조회가 복사, 회복 뒤 count 만 올려 A×2 커밋·사본 B×2·0 반환 | 스크래치 C 로 재현: `committed A x2`, `caller copy B x2`, 반환 0 | ✅ 채택 — 회복한 저장은 −1 로 건너뜀. 같은 재현이 `A x1`·−1·사본 그대로. 하네스에 시나리오 추가, "건너뛰지 않음" 변이 검출 |
| Q1 이 정당한 바깥 트랜잭션을 버릴 일은 없다 (전수 검색) | 내 전수 grep 과 같음 (`cache_backend_sqlite.c:219`, `http_server.c:406`, `trans_cache.c:212`) | ✅ 사실 |
| `State` 에 hint mixed·version·시각이 없고 사본은 번역·count 만 비교 | `tests/test_fault_store.c` 열람 | ✅ 채택 — 비교 확장. 그 결과 **새 불일치 발견**: 사라진 행을 다시 넣고 hint 단계가 실패하면 커밋은 hint 없음인데 사본에 옛 hint 가 남음 (`cache_backend_sqlite.c` 재삽입 경로 + `if (hint_written)` 만 반영). 서버는 저장 직후 사본을 버려 응답·캐시엔 영향 없지만 "커밋되면 사본 = 커밋 상태" 계약 위반 → 고침 (`StagedParent.reinserted`), 변이 검출 |
| `read_state()` 가 부모 없는 hint 를 못 보고 SQL 오류를 "행 없음" 으로 삼킨다 | 그 함수 열람 | ✅ 채택 — 오류는 중단, 부모 없는 hint 수를 매번 0 으로 단정 |
| 할당 훑기가 text·C 할당에도 hint 실패를 허용하고 0·−1 외 반환도 받는다 | 훑기·verify 열람 | ✅ 채택 — hint 실패 허용은 SQLite 자체 할당만, 반환은 0/−1 만 |
| 사라진 행 고정물이 없다 (계획 F5 에 적고 빠뜨림) | 고정물 열람 | ✅ 채택 — 두 백엔드에 추가 (SQLite 는 다른 연결로 DELETE, text 는 배열에서 제거) |
| 빌더 훑기가 NULL 을 강제하지 않고 `single` 을 안 보며, `seen`·authorizer 발동 횟수를 단정하지 않는다 | 두 하네스 열람 | ✅ 채택 — 실패하면 반드시 NULL, `single` 단정, k번째 실패가 실제로 주입됐는지, 규칙마다 발동 확인 (이 검사가 변이 2개를 먼저 잡음) |
| text 주석의 "모든 할당이 그 검사 전" 은 틀림 | `cache_backend_text.c:487`, `:338-370` 열람 | ✅ 채택 (내 오류) — 주석 정정 |
| `-d` 확인이 약하고, 종료 뒤 포트 확인은 "바인드 안 함" 증거가 아니다 | `tests/test_fault_server.py` 열람 | ✅ 채택 — `-d` 도 두 줄 순서·listening 없음, 포트 확인은 문구를 사실대로 |
| DEBUG 빌드면 "mkdtemp 로그 없음" 검사가 실패 | 그 검사 열람 | ⚖️ 사실이나 make 대상은 일반 빌드 — 가정을 시험 설명에 적음 |
| 하네스 `fopen()` 결과 미확인 | 열람 | ✅ 채택 |
| Q2·SQLite 할당자 설치 시점·매크로·authorizer 다중 규칙 세기는 문제없음 | 내 검증과 같음 (시험 통과·변이 검출) | ⏭️ |
