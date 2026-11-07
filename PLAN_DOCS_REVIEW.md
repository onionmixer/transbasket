# 문서 검토 2026-11-06 (main `138e392` 기준)

요청: "문서들을 검토해서 내용의 update 가 필요한 문서들을 문서 update 작업을 진행". 원칙: 지적은 전부 코드를 열거나 실측해서 판정,
계산은 python, codex 는 입력일 뿐. **현재 동작을 설명하는 문서**(README, INSTALL, CLAUDE.md, `transbasket_curl_sample.txt`, 추적 설정 템플릿
4개, `cache_sqlite_schema.sql`) 는 코드에 맞춘다. **이력 문서**(`PLAN_*.md` 본문, TODO, SESSION_SUMMARY, TEST_RESULTS 의 지난 절) 는
당시 기록이므로 머리말의 현재 상태 서술만 본다.

## 방법

1. 기계 검사 (python): 문서가 말하는 파일·함수·`make` 타깃·설정 키가 코드에 있는지, 로더가 읽는 키가 README 에 있는지,
   따옴표 값 뒤에 주석이 붙은 설정 예시 (2026-11-06 부터 기동 오류).
2. 실측: `./transbasket -h`, `/health` 응답, 인라인 주석 설정의 기동 실패 (rc=1), 경로 기준 (실행 파일·설정·cwd 를 서로 다른 디렉터리에).
3. codex 독립 감사 (편집 전) — 제 목록 7건 검토 + 새 지적 28건. 전부 해당 코드 줄을 열어 확인했다.

## 판정 (수정 대상)

| # | 문서 위치 | 틀린 서술 → 실제 (근거) | 판정 |
|---|---|---|---|
| 1 | README:151,197 (+205-213) | 따옴표 값 뒤 주석이 붙은 예시 → 그대로 쓰면 기동 실패 (실측 rc=1, `config_loader.c:85-92`) | ✅ 주석을 앞 줄로 |
| 2 | README:147, :250; INSTALL:66 | 상대 경로가 실행 파일 / 설정 파일 하나를 기준으로 풀린다 → 설정 파일·언어표는 실행 파일, 프롬프트·role 은 설정 파일, 캐시·`CODEX_RUN_DIR`·trace 는 cwd (실측, `config_loader.c:408,781`, `language_table.c:166-193`). `-p` 기본은 `../PROMPT_PREFIX.txt` 가 아니라 `PROMPT_PREFIX_FILE` (기본 `PROMPT_PREFIX.txt`) | ✅ (README:147 은 설정 파일 자체에 대해서는 맞음 — 범위만 분명히) |
| 3 | README (전체) | 캐시 키 6개와 `FREQUENCY_PENALTY`·`PRESENCE_PENALTY` 설명 없음, INSTALL:4 는 "키 전체 목록은 README" (python: 로더 키 41개 대 README) | ✅ 설정 참고 표 추가 |
| 4 | README:331-346 | `-r`·`-d`·`-l` 없음, `-p` 기본값 틀림 (`./transbasket -h`, `main.c:63-70`) | ✅ |
| 5 | README:340,345; INSTALL:74; CLAUDE.md:494 | `-w`/`MAX_WORKERS` = 스레드 풀 → 연결마다 스레드 (`http_server.c:598`), 값은 `CODEX_MAX_CONCURRENCY`·`INFLIGHT_MAX_KEYS` 기본값 (`:450-465`) | ✅ |
| 6 | 템플릿 3개의 `TRANS_CACHE_SQLITE_JOURNAL_MODE`/`SYNC` | 설정 가능 → 로더만 읽고 쓰는 곳 없음, `apply_pragmas(db, "WAL", "NORMAL")` 고정 (`cache_backend_sqlite.c:448`, grep) | ✅ "현재 무시됨" 명시 (코드 문제는 보고) |
| 7 | `transbasket_codex.conf:77` | mongodb/redis 를 백엔드처럼 나열 → 미구현, text 로 대체 (`config_loader.c:604-611`) | ✅ |
| 8 | README:208,648; 템플릿 2개 | `CODEX_TIMEOUT` "120 미만이어야" → 120 이상은 110 으로 줄이고 경고, 잘못된 값·0 이하는 100 (`config_loader.c:741-748,860-865`) | ✅ |
| 9 | README:487,585 | codex 결과 "길이 제한 없음" → 4 MB (`CODEX_OUT_MAX`, `codex_exec.c:27`) 넘으면 502 | ✅ |
| 10 | README:241-244; INSTALL:148-149; CLAUDE.md:216; 템플릿 2개 | 임계값 2 이상이면 "절대" 캐시 안 됨 → 결과가 같으면 count 가 오르고 결과가 바뀌면 1 로 (`cache_backend_sqlite.c:721-736`). 실측 (임계값 3, 4요청 모두 재호출) 은 그대로 둠 | ⚖️ "절대"→"사실상" 로 완화, 권고 1 유지 |
| 11 | INSTALL:146-147 | `CODEX_RUN_DIR` 이 없으면 mkdtemp 실패 → 기동 시 0700 으로 만들어 보고 실패하면 경고 (`http_server.c:431-440`) | ✅ |
| 12 | CLAUDE.md:213 | 요청마다 반드시 새 cwd → best effort, 실패하면 cwd 없이 실행 (stdout 결과), structured 는 cwd 필요 (`backend_codex.c:257-269`) | ✅ |
| 13 | curl_sample:239-244 | 503 본문 `Translation failed` → 실제 원인 문구 (`Server error`, curl 오류 문구, `Translation deadline exceeded`; `backend_normal.c:548-572`, `http_server.c:337-338`) | ✅ |
| 14 | INSTALL:106; curl_sample:360-361; CLAUDE.md:123,423 | 응답은 항상 uuid/timestamp 보존 → 성공 응답만. 오류 응답의 timestamp 는 현재 시각 (`json_handler.c:237-240`), 422 는 uuid 없음 (`http_server.c:205-209`) | ✅ |
| 15 | README:483; CLAUDE.md:423 | timestamp 그대로 반환 → 63바이트까지만 저장 (`json_handler.h:9`, `json_handler.c:66`), 소수점 자릿수 제한 없는 정규식이라 더 길면 잘림 | ✅ 사실 명시 (코드 문제는 보고) |
| 16 | README:18,396,732; CLAUDE.md:351-359 | "RFC 3339 검증" → 형식만 보는 정규식, 날짜·시각 범위와 소문자 `t`/`z` 는 검사 안 함/거부 (`utils.c:269-284`) | ✅ |
| 17 | CLAUDE.md:375 | 5xx·타임아웃만 재시도 → curl 전송 오류 전반 재시도, 4 MB 초과·할당 실패는 제외 (`backend_normal.c:524-545`) | ✅ |
| 18 | CLAUDE.md:378 | "UUID ensures idempotency" → 병합·캐시 키에 uuid 없음 (`inflight_translation.c:42-55`, `trans_cache.c:35-40`), 상관용일 뿐 | ✅ |
| 19 | CLAUDE.md:604 | `curl $OPENAI_BASE_URL/v1/models` → base 가 이미 `/v1` (`backend_normal.c:392`), INSTALL 처럼 `/models` | ✅ |
| 20 | CLAUDE.md:674 | API 키를 환경 변수로 → 설정 파일에서만 읽음 (getenv 는 `TRANSBASKET_CONFIG`·`MAX_WORKERS` 뿐, grep) | ✅ |
| 21 | README:637-638; INSTALL:365-366 | trace 에 원문·번역문 전체 → normal trace 는 요청 본문 (curl 명령) 만, codex trace 는 프롬프트·결과·stderr (`backend_normal.c:81-105`, `backend_codex.c:106-120`) | ✅ |
| 22 | README:869 | libcurl 연결 풀 → 시도마다 핸들 생성·파기 (`backend_normal.c:382-384,522`) | ✅ 삭제 |
| 23 | README:50-57,860-861 | macOS 설치, "POSIX compliant / No GNU extensions" → Linux 전용 (`/proc/self/exe`, `pipe2`, `posix_spawn_file_actions_addclosefrom_np`) | ✅ |
| 24 | README:862 | 오류는 stderr → 로그는 stdout (`utils.c:145`), 일부 파서 진단만 stderr | ✅ |
| 25 | `cache_sqlite_schema.sql` | 실행 시 스키마와 같다 → 스크립트에만 `CHECK(last_used >= created_at)`, hint 테이블·트리거·뷰는 기동 시 생성, PRAGMA 값 다름 (`cache_backend_sqlite.c:37-52,163-210,267-306`) | ✅ 머리말·주석으로 차이 명시 (SQL 은 그대로) |
| 26 | `cache_sqlite_schema.sql:15`; 예시 설정 2개 | count = 요청 수 / "hit count" → 같은 번역 결과가 나온 횟수 + 캐시 사용, 결과가 바뀌면 1 (`http_server.c:280,347`, `cache_backend_sqlite.c:721-736`) | ✅ |
| 27 | CLAUDE.md:263 | 시험 훅은 structured-hint 시험 빌드에만 → `test-codex-backend` 빌드도 (`Makefile:182,205`) | ✅ |
| 28 | CLAUDE.md:303 | "Tests bind loopback only" → 네임스페이스 안에서 198.18.0.1 도 씀 (같은 문장 안의 모순) | ✅ |
| 29 | CLAUDE.md:398; INSTALL:122 | 실제 codex 의 auto 미검증 → TEST_RESULTS source_hint 절: 실제 codex 32건 (기준언어 24/24, 구조화 실패 0, 오염 0), 번역 품질 채점은 안 함, TranslateGemma 실측은 503 그대로 | ✅ |
| 30 | README:646; README /translate 상태 코드 | "초기화" (MCP initialize) 잔존, codex 502 사유 목록이 옛것 | ✅ |
| 31 | CLAUDE.md:593 | "프로젝트 루트에서 실행" → cwd 는 설정 탐색과 무관 | ✅ |
| 32 | `PLAN_SOURCE_HINT_METADATA.md:5` (머리말) | 병합이 "미커밋" → `8de09fa` 로 커밋됨 | ✅ "작성 당시" 표시 |

| 33 | INSTALL:225 | 주석 처리된 예시 `#INFLIGHT_MAX_KEYS="30"          # …` — 주석을 풀면 기동 오류 (편집 후 재검사에서 발견) | ✅ 주석을 앞 줄로 |

수정하지 않음: TODO.md·SESSION_SUMMARY.md 머리말 보강 (이미 이력 문서라고 밝히고 현재 문서를 가리킴 — codex 와 같은 판단).

## 코드 문제 (문서 작업 범위 밖, 보고만)

> 2026-11-07: 아래 4건은 [PLAN_CODE_ISSUES_4.md](PLAN_CODE_ISSUES_4.md) 로 수정했다. 위 판정표의 해당 문서 문구도 그때 바꿨다.

- `TRANS_CACHE_SQLITE_JOURNAL_MODE`/`SYNC` 를 읽기만 하고 적용하지 않음 (`cache_backend_sqlite.c:448` 고정값).
- `timestamp` 는 길이 제한 없이 검증한 뒤 63바이트로 잘라 저장 (`json_handler.c:66`) — 응답이 요청과 달라질 수 있음.
- timestamp 검증은 형식만 (`utils.c:269-284`).
- `config_loader.c:785` 가 찾은 경로 대신 `-p` 인자 (`-p` 가 없으면 NULL) 를 `%s` 로 기록.

## 편집 후 검증

- 기계 검사 다시: 남은 3건은 오탐 (`::ffff:a.b.c.d` 표기, 셸 변수 `SAMPLE_TEXT`·`JSON_PAYLOAD`). 로더가 읽는 키 41개가 모두 README 에 있음.
- 따옴표 값 뒤 주석 검사를 주석 처리된 예시까지 넓혀서: 0건 (INSTALL:225 를 고친 뒤).
- 표 모양 (열 수) 검사: 이상 0.
- README 의 고친 예시 블록으로 설정을 만들어 기동: normal 블록 기동, codex 옵션 블록은 주석을 모두 풀어도 문법 오류 없이 기동.
- `make test-config-syntax`: 추적 템플릿 4개 그대로 기동.
- `sqlite3 <새 DB> < cache_sqlite_schema.sql`: 오류 없이 객체 9개.
- 인용한 libcurl 문구는 `curl_easy_strerror(7)`·`(28)` 로 확인, 전송 오류 → 503 은 `http_server.c:311-312`.
