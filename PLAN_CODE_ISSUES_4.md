# 코드 문제 4건 수정 계획 (문서 검토에서 발견)

작성 2026-11-07. 작업 위치: worktree `../transbasket-fix4`, 브랜치 `fix/code-issues-4` (main `c9dab19`).
요청: "코드문제 4 건에 대한 수정을 진행". 원칙: 계산·기대값은 python, codex 는 입력일 뿐 (코딩 전 계획 검토), 별도 worktree, 모든 기준 통과 후
main fast-forward. `파일:줄` 은 `c9dab19` 기준. 출처는 [PLAN_DOCS_REVIEW.md](PLAN_DOCS_REVIEW.md) 의 "코드 문제" 4건.

## 1. SQLite `TRANS_CACHE_SQLITE_JOURNAL_MODE` / `TRANS_CACHE_SQLITE_SYNC` 가 적용되지 않음

현재: 로더가 읽어 `Config` 에 넣지만 쓰는 곳이 없다 (grep). `sqlite_backend_init(db_path)` (`cache_backend_sqlite.c:396`) 가
`apply_pragmas(db, "WAL", "NORMAL")` (`:448`) 로 고정. `trans_cache_init_with_backend(type, path, options)` 의 `options` 는 미사용
(`trans_cache.c:58`). 로더는 **이미** 허용 목록으로 검사한다 (`config_loader.c:624-643`: journal 6개, synchronous `OFF/NORMAL/FULL/EXTRA`,
대소문자 무시, 목록 밖이면 경고 후 `WAL`/`NORMAL`) — 설정 문자열이 SQL 에 들어간 적은 없다 (codex 지적, 열람 확인; 처음 계획의 서술은 틀림).
`apply_pragmas` 는 `PRAGMA journal_mode` 가 돌려주는 실제 모드를 보지 않는다.
배포 영향: 값을 적은 추적 템플릿 3개 (`transbasket.conf`, `transbasket_codex.conf`, `transbasket_example_cache_sqlite.conf`; text 예시는 키 없음) 와
배포 설정 3개 (`transbasket.local.conf`·`transbasket_dkfs_codex.conf`·`transbasket_pge4_codex.conf`, grep) 가 모두 `WAL`/`NORMAL`, 실제 DB 3개도 `wal`
(sqlite3 로 확인) → 정상 값을 쓰는 지금 운영에서는 pragma 동작 변화 없음 (로그 줄은 바뀜, S4).

SQLite 실측 (3.37.2, python sqlite3): `journal_mode=<X>` 는 실제 모드를 소문자 한 줄로 돌려준다. 다시 열면 `wal` 만 유지되고 나머지는 `delete`.
`journal_mode=bogus` 는 오류 없이 현재 모드를 돌려주고, 메모리 DB 의 WAL 요청은 `memory`. `synchronous` 는 연결 단위 (다시 열면 기본 2),
`bogus` 는 조용히 1, `4` 는 4 로 남음 → 값을 그대로 넘기면 안 되고, 다른 연결로는 실효값을 확인할 수 없다.

| # | 결정 (권장안) | 근거 |
|---|---|---|
| S1 | 받는 값은 **지금 로더의 허용 목록 그대로** (숫자 `0`~`3` 은 지금처럼 받지 않음). 백엔드는 다시 한 번 같은 목록으로 확인하고 정규화한 대문자 이름만 SQL 에 쓴다 (방어) | 받는 범위를 바꾸지 않는다 |
| S2 | 목록 밖의 값은 **지금처럼 경고 후 기본값** (`WAL`/`NORMAL`) — 새 기동 오류를 만들지 않는다 | 잘못된 값은 이미 경고된다. 결함은 올바른 값이 적용되지 않는 것뿐 (codex 지적으로 처음 계획의 "기동 오류" 를 거둠) |
| S3 | `PRAGMA journal_mode` 가 **성공했는데** 결과가 요청과 다르면 (대소문자 무시) `Warning: SQLite journal_mode is X, not the configured Y` 후 계속. PRAGMA 가 **SQL 오류**면 지금처럼 캐시 초기화 실패 (`cache_backend_sqlite.c:274-287,448-454`) → 서버는 캐시 없이 계속 (`http_server.c:496-498`) | 거부 (다른 모드 반환) 와 오류는 다른 경우 (codex 지적) |
| S3a | 순서는 지금 그대로: 스키마·이전 (`apply_schema()` 의 `BEGIN IMMEDIATE` `:219` … `COMMIT` `:251`) 이 끝난 뒤 pragma | pragma 를 앞당기면 이전 단계의 잠금·내구성이 바뀐다 |
| S4 | pragma 를 적용한 직후 (`SQLite cache initialized: <path>` **앞**) 에 `SQLite journal_mode=<실제>, synchronous=<이 연결의 값>` 한 줄. `synchronous` 를 다시 읽지 못하면 SQL 오류처럼 캐시 초기화 실패 | 실효값을 운영자가 볼 수 있게 (처음 계획은 "다음에" 였으나 구현 순서대로 정정) |
| S5 | `cache_tool`·시험 하네스는 `options=NULL` → 지금처럼 `WAL`/`NORMAL` | `cache_tool` 은 서버 설정을 읽지 않는다. 파일에 남는 것은 WAL 뿐이므로, 서버를 WAL 이 아닌 값으로 쓰면 `cache_tool` (migrate·backfill dry-run 포함, 스키마 초기화도 함) 이 DB 를 WAL 로 **바꾸려 한다** — 문서에 적는다. 서버는 다음 기동 때 다시 설정값을 요청 |
| S6 | `MEMORY`·`OFF`·`synchronous=OFF` 는 허용하되 기동 시 `Warning: ... can corrupt the cache on a crash` | SQLite 가 허용하는 값이지만 위험 |

## 2. `timestamp` 가 63바이트에서 잘림 / 3. timestamp 검증이 형식만

현재: `validate_timestamp()` (`utils.c:269-284`) 는 정규식 `^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?(Z|[+-][0-9]{2}:[0-9]{2})$`
(호출마다 `regcomp`). 범위를 보지 않아 `2026-99-99T99:99:99Z` 가 통과하고, 소수점 자리 수 제한이 없는데 `json_handler.c:66` 이
`timestamp[64]` 에 63바이트까지만 복사 → 더 긴 값은 잘린 채 응답·trace 파일 이름에 쓰인다. 호출하는 곳은 `json_handler.c:57` 하나 (grep).
같은 구조체의 `uuid[37]` 은 정규식이 36자로 고정, 언어 코드는 정규화 코드라 잘림 없음 (확인).
길이 (python): 고정부 19바이트 → 63바이트 안에 소수점 자리 `Z` 이면 42개, `+HH:MM` 이면 37개. 흔한 가장 긴 형태 (나노초+`+09:00`) 35바이트.

| # | 결정 (권장안) | 근거 |
|---|---|---|
| T1 | 63바이트를 넘는 timestamp 는 **422** (HTTP 본문은 기존 `VALIDATION_ERROR`/`Request validation failed`, 로그는 `Error: Invalid timestamp format: ...`) — 자르지 않는다 | 응답이 요청과 다른 값을 돌려주는 것이 결함. 동적 할당보다 변경이 작고, 63바이트면 실사용에 충분 |
| T2 | RFC 3339 범위 검사: 월 01-12, 일 01-그 달의 마지막 날 (그레고리력 윤년: 4로 나누어지고 100으로 나누어지지 않거나 400으로 나누어짐, 연도 0000 은 윤년), 시 00-23, 분 00-59, 초 00-60, 소수부는 `.` 뒤 숫자 1개 이상, 오프셋 `Z` 또는 `±HH:MM` (시 00-23·분 00-59, `-00:00` 그대로 허용) | 형식만 보는 검증이 결함. **:60 은 문법 수준으로만** 허용 — RFC 3339 §5.7 은 실제 윤초 시각 (오프셋 반영) 에만 허용하므로 이 검증은 RFC 보다 느슨하다고 문서에 적는다 |
| T3 | `T`·`Z` 소문자, 날짜와 시각 사이 공백은 **지금처럼 거부** | RFC 3339 는 소문자를 허용하지만, 이번 수정은 받아들이는 범위를 넓히지 않는다 (좁히기만) |
| T4 | 정규식 대신 위치 고정 파서 (숫자만, 정확한 구분자). 같은 결과를 내는 python 명세 `tests/timestamp_ref.py` 와 차등 시험 | 다른 검증기 (strict_json, LISTEN) 와 같은 방식. 호출마다 `regcomp` 하던 비용도 없어짐 |
| T5 | 동작 변화: 지금까지 받던 불가능한 날짜·시각·오프셋, 63바이트를 넘는 (RFC 로는 올바른) timestamp 가 이제 422. 검증이 캐시 조회·병합보다 먼저라 (`http_server.c:201-209,277-286`) **캐시에 있는 원문이어도** 422. 63바이트 이하의 올바른 값은 지금처럼 바이트 그대로 돌려줌 | 문서·업그레이드 노트에 적는다 |

## 4. 프롬프트 경로 오류 로그가 `(null)`

`config_loader.c:785` 가 `resolve_path()` 에 실제로 넘긴 `actual_prompt_path` (`:781`) 대신 `prompt_prefix_path` (`-p`, 없으면 NULL) 를 `%s` 로 기록.
→ `actual_prompt_path` 를 기록 (NULL 이면 `(unset)`). 같은 함수의 role 로그 (`:818`) 는 NULL 이면 `ROLS.txt` 로 바뀐 값 (`:393-395`) 을, auto 파일 로그 (`:806`) 는 넘긴 값을 그대로 기록하므로 문제 없음 (열람 확인).

## 4a. 사용자 결정 (2026-11-07)

T1 "422 로 거부", T3 "지금처럼 거부" — 둘 다 권장안. S2 는 codex 검토 후 "지금처럼 경고" 로 바뀌어 묻지 않음 (새 동작 변화 없음).

## 5. 검증

| # | 대상 | 방법 |
|---|---|---|
| V1 | 1 | **백엔드 자신의 연결**을 조회하는 C 하네스 (`ctx->db`, 선례 `tests/test_source_hint_store.c:216-219`): journal 6개 × synchronous 4개 (대소문자 섞어), options=NULL 이면 `wal`/`1`, 성공했지만 다른 모드 (메모리 DB 의 WAL 요청 → `memory`) 는 경고, 다시 열기 후 WAL 만 유지. + 서버 종단 (python): 설정 → 기동 로그의 `SQLite journal_mode=..., synchronous=...` 줄, 목록 밖 값·SQL 조각 (`WAL; DROP TABLE trans_cache`) 은 지금처럼 경고 후 `wal`/`NORMAL` 이고 DB 의 행 수 그대로, 키 순서 (마지막 줄 우선). 수정 전 바이너리는 설정과 무관하게 `wal`/`NORMAL` (로그 줄 없음) |
| V2 | 2·3 | 신규 `tests/timestamp_ref.py` (명세) + C 하네스 + `tests/test_timestamp.py`: 경계 사례 (윤년 1900/2000/2024/2100/0000 의 02-29, 30일 달의 31일, 24:00, :60, 오프셋 ±23:59/24:00, 63/64바이트, 소문자, 공백, 빈 소수부) + 무작위 수만 건에서 C = 명세, 돌연변이 (윤년 규칙 틀림, 63 경계 ±1 등) 검출 |
| V3 | 2 | 서버 종단: 63바이트 timestamp (`Z` 형과 `±HH:MM` 형 각각) 는 200 이고 응답·DEBUG trace 파일 이름에 그대로, 64바이트는 422, 불가능한 날짜는 422, 캐시에 있는 원문도 timestamp 가 틀리면 422, 서로 다른 timestamp 의 병합 owner/waiter/캐시 적중이 각자 자기 값을 받음 (normal·codex 각각) |
| V4 | 4 | **없는 상대 경로**로 (절대 경로면 다른 오류 분기, `config_loader.c:201-215`) `-p` 있을 때·없을 때 로그에 시도한 경로가 나옴 (`(null)` 아님) |
| V5 | 회귀 | `make test-*` 전부, `test_config_limits.py`, 프롬프트 회귀, parity (`test-backend-parity`, 기준 `c9dab19`), ASan, valgrind |
| V6 | 배포 | 배포 설정 3개 사본 (캐시 경로는 사본 디렉터리) 기동: 캐시 초기화 성공 줄, 경고 0, journal 줄 `wal`/`NORMAL` |

## 6. codex 계획 검토 판정 (코딩 전)

| codex 주장 | 내 검증 방법 | 결과 |
|---|---|---|
| 로더가 이미 허용 목록 검사 (목록 밖은 경고 후 기본값), 설정 문자열이 SQL 에 들어간 적 없음 | `config_loader.c:624-643` 열람 | ✅ 채택 — 처음 계획의 S1/S2 서술이 틀림 (내 오류). S2 는 "기동 오류" → "지금처럼 경고" 로 |
| 캐시 초기화 실패 시 서버는 캐시 없이 계속 | `http_server.c:496-498` 열람 | ✅ (S2 를 경고로 바꿔 해당 없음, S3 오류 경로 설명에 반영) |
| 거부 (다른 모드 반환) 와 SQL 오류를 구분해야 | 실측 (`bogus` → 현재 모드, 메모리 DB → `memory`), `:274-287` 열람 | ✅ 채택 (S3) |
| 스키마·이전이 pragma 보다 먼저, 순서 유지 | `cache_backend_sqlite.c:437-448` 열람 | ✅ 채택 (S3a) |
| 파일에 남는 것은 WAL 뿐, synchronous 는 연결 단위 | 실측 (다시 열기) | ✅ 채택 (S5 문구, V1 설계) |
| SQLite 는 접두사·`ON/YES/TRUE` 도 받음 — 좁은 목록은 의도된 정책 | 로더 목록 열람 (그 값들은 지금도 거부) | ✅ (S1: 지금 목록 유지) |
| V1: 다른 연결로는 synchronous 를 볼 수 없음, 백엔드 연결을 조회해야 | 실측 + `test_source_hint_store.c:216-219` 선례 열람 | ✅ 채택 |
| T2 범위는 맞음, `-00:00` 유지, `:60` 은 RFC §5.7 보다 느슨 | RFC 3339 §5.6-5.7 (codex 인용) — 문법은 내 T2 와 같음 | ✅ 채택 (문구) |
| T1 은 모든 소비처와 맞음 (응답, trace 이름 최대 100바이트, 캐시·병합은 timestamp 를 키로 안 씀) | `trace_file.c:74-77`, `trans_cache.c:35-39`, `inflight_translation.c:42-55` 열람 | ✅ |
| 검증이 캐시 조회보다 먼저 → 캐시 적중도 422 | `http_server.c:201-209,277-286` 열람 | ✅ 채택 (T5) |
| `Invalid timestamp format` 은 로그 문구, HTTP 본문은 `VALIDATION_ERROR` | `json_handler.c:60`, `http_server.c:205-209` 열람 | ✅ 정정 (내 오류) |
| 값을 적은 추적 템플릿은 4개가 아니라 3개 | grep (text 예시엔 키 없음) | ✅ 정정 (내 오류) |
| 4번: 다른 NULL-`%s` 없음 | `load_config` 의 로그 호출 열람 | ✅ |
| V3·V4·V5·V6 보강 (trace 이름, 병합/캐시 적중, 상대 경로, 캐시 초기화 성공 확인) | 계획 대조 | ✅ 채택 |

### 구현 diff 검토 (커밋 전)

| codex 주장 | 내 검증 방법 | 결과 |
|---|---|---|
| `synchronous` 다시 읽기 실패를 넘어가 `unknown` 으로 계속, OFF 경고도 빠질 수 있음 | `cache_backend_sqlite.c` 의 읽기 블록 열람 | ✅ 채택: 실패하면 `-1` (SQL 오류와 같은 경로) |
| 병합 시험이 0.2 s 간격 대 1.5 s 지연이라 부하에서 흔들릴 수 있음 | 시험 열람 | ✅ 채택: normal 은 3 s, codex 는 게이트 파일로 순서를 고정 |
| 캐시 적중을 실제로 확인하지 않음, codex 병합 없음 | 시험 열람 | ✅ 채택: 엔진 호출 수 (upstream 요청·codex exec 실행) 로 캐시 저장·적중 확인, codex 병합 추가 |
| "month 13" 돌연변이는 `days_in_month[12]` 를 읽는 정의되지 않은 동작 | `utils.c` 의 배열 색인 열람 | ✅ 채택: "day range not checked" 로 교체 (10/10 검출 유지) |
| 하네스 종료 코드·stderr 무시 | `test_timestamp.py`·`test_sqlite_pragmas.py` 열람 | ✅ 채택: 0 이 아니거나 stderr 가 있으면 실패 |
| S4 순서가 계획과 반대, 주입 시험이 빈 DB 라 행 보존을 못 봄 | 로그 순서·시험 열람 | ✅ 채택: 계획 문구 정정, 행을 미리 넣은 DB 로 주입 시험 (행 1 유지) |
| timestamp 파서 경계·의미, pragma 문장 수명, 옵션 수명, `:785` 수정 | 내 재검토와 같음 | ✅ (문제 없음) |
