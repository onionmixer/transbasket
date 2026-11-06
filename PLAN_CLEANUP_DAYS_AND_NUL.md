# cleanup 기간 정수 오버플로 · `\u0000` 잘림 수정 계획

작성 2026-11-06. 출발점: `PLAN_SOURCE_HINT_METADATA.md` §14 의 미해결 결함 2건.
작업 위치: worktree `../transbasket-fix-nul`, 브랜치 `fix/cleanup-days-nul` (main `9c4036a` 기준).
원칙: 계산은 python, codex 는 입력일 뿐이며 지적은 전부 재현·코드로 확인한 뒤 채택, push 없음.
이 문서의 `파일:줄` 인용은 모두 수정 전 커밋 `9c4036a` 기준이다 (자동 대조로 확인).

## 1. 실측 (현재 main 바이너리, 2026-11-06)

모든 수치는 스크래치 스크립트로 측정했다 (`scratchpad/nul/probe.py`, `cleanup_probe.py`, `cjson_probe.py`).

### 1.1 cleanup 기간

`days * 24 * 60 * 60` 이 `int` 로 계산된다: `cache_backend_text.c:598,638`, `cache_backend_sqlite.c:882,939`,
주기 `http_server.c:83`. 부호 있는 오버플로는 C 에서 미정의 동작이다. 설정값은 `atoi()`(`config_loader.c:541`),
`cache_tool cleanup <days>` 도 `atoi()`(`cache_tool.c:1059`) 이며, 범위 밖 `atoi` 역시 미정의 동작이다.

`cache_tool -f <text> cleanup <days>`, 항목 3개(1일·100일·400일 전 사용):

| days | 남은 항목 | 옳은 결과 | 32비트 wrap 모델 |
|---|---|---|---|
| 30 / 365 / 24855 | 1 / 2 / 3 | 1 / 2 / 3 | 일치 |
| 24856 / 36500 / 49710 | **0 / 0 / 0** | 3 / 3 / 3 | 일치 |
| 49711 | **0** | 3 | 일치 (wrap 결과가 +63,104초 → 임계가 17.5시간 전) |
| 60000 | 3 | 3 | 일치 |
| 99999999999 | **0** | (범위 밖) | 일치 (glibc `atoi` = `(int)strtol` = 1,215,752,191) |
| `30days` | 1 | (형식 오류) | `atoi` 가 30 으로 읽음 |

python 계산: 안전 상한 `INT_MAX / 86400 = 24,855`일. 임계가 미래가 되는 구간(≤400,000일)은
24,856–49,710, 74,566–99,420, 124,276–149,130, … 이고, 그 경계 직후(49,711 등)는 작은 양수로 wrap 되어 역시 거의 전부 삭제된다.
`days = INT_MAX` 라도 64비트 곱은 185,542,587,100,800 으로 `int64` 범위 안이다.
`now / 86400 = 20,762` 이므로 20,762일 이상은 의미상 "아무것도 만료되지 않음" 이다.
모든 배포 설정(템플릿 포함)은 `60` 이라 현재 운영 영향은 없다.

같은 종류로 `cache_tool delete <id>` 의 `atoi` 를 확인했다: `delete 4294967297` 이 **id 1 을 삭제했다** (`1abc` 도 1).

### 1.2 `\u0000` 잘림 — 레거시 MCP 만이 아니다

cJSON 1.7.15 를 ctypes 로 직접 호출한 결과 (`cjson_probe.py`):
- `\u0000` 그리고 **뒤 4바이트가 전부 16진수가 아닌 모든 `\u`** (`\u00ZZ`, `\u12ZZ`, `\uZZ12`, `\u-123`, `\uD8ZZ`) 가 U+0000 으로 디코드되어 `valuestring` 이 거기서 끊긴다.
- 키도 같다: `{"te\u0000xt":"v"}` → 키 `te`. 따라서 `"text\u0000junk"` 키는 `text` 로 조회된다.
- 루트 값 뒤의 내용은 무시된다: `{"k":"v"} \u0000 junk` → 성공.
- 원시 NUL 바이트가 문자열 안에 있으면 파싱 실패 (C 문자열로 읽으므로). 즉 원시 NUL 은 조용한 잘림을 만들지 않는다.
- `\uD83D` 단독, `\uDE00` 단독, `\uD83D\u0000` 은 파싱 실패. `\u00` (짧음) 실패.

서버 종단 측정 (`probe.py`, 가짜 codex/업스트림):

| 경로 | 입력 | 현재 결과 |
|---|---|---|
| MCP `structuredContent.content` | `앞\u0000뒤` / `앞\u00ZZ뒤` | 200, `앞` 응답·캐시 |
| MCP `content[].text` | `앞\u0000뒤` | 200, `앞` 응답·캐시 |
| exec plain | `-o` 파일에 원시 NUL 바이트 | 200, `앞` 응답·캐시 (`result_len` 은 알지만 쓰지 않음) |
| normal 백엔드 | `content` 에 `\u0000` | 200, `앞` 응답·캐시 |
| **요청 `text`** | `원문\u0000숨김` / `원문\u00ZZ숨김` | 200, 업스트림에 `원문` 만 전달, `원문` 키로 캐시 |
| 요청 `uuid` | `<uuid>\u0000junk` | 200 (검증 통과) |

structured 모드는 이미 `strict_json_validate()` 가 막는다 (SJ_NUL/SJ_SYNTAX). `unescape_string()` 은 `\u` 를 처리하지 않음을 코드로 확인했다.

## 2. 범위

포함:
- A. cleanup 기간 계산·파싱 (서버 설정, 주기, 두 백엔드의 cleanup/stats, `cache_tool cleanup`).
- A'. `cache_tool delete <id>` (같은 `atoi` wrap 으로 엉뚱한 항목 삭제 — §1.1). **결정 필요 (§7 Q1)**.
- B. cJSON 이 NUL 을 만들어 내는 입력을 cJSON 전에 막는다: MCP 응답, normal 응답(스트림 포함), 요청 본문.
- B'. exec plain 결과의 원시 NUL 바이트.
- 요청 거부(B 의 요청 부분)는 지금까지 200 이던 요청이 422 가 되는 API 동작 변화다. **결정 필요 (§7 Q2)**.

제외 (기록만):
- 텍스트 캐시 파일의 `\u0000`: 우리 기록기는 C 문자열만 쓰므로 만들 수 없다. 손으로 고친 파일에서만 생긴다.
- MCP 줄 안 원시 NUL: 값이 끝나기 전의 NUL 이면 파싱 실패 → 해당 줄 폐기 → 마감까지 대기(504).
  완결된 루트 값 **뒤**의 NUL 이면 cJSON 이 그 앞까지를 받아들이고 나머지를 무시한다 (§8 R6 실측: 60바이트 중 55 소비). 어느 쪽도 값의 잘림은 아니다.
- 스트림 모드의 파싱 실패 청크 무시, `MAX_STREAM_BUFFER` 무음 중단, 청크 `malloc` 실패 시 같은 청크 재시도 루프: 기존 동작.
- `PORT`·`SEED`·`TRANS_CACHE_THRESHOLD` 의 `atoi`: 파괴적이지 않음. 별도 과제.
- 코드포인트 U+0001 등 다른 제어문자: 잘림이 아니다.

## 3. 설계 A — cleanup 기간

A1. `trans_cache.c` 에 순수 함수 2개 (헤더 공개, 테스트 대상):
```c
int64_t trans_cache_cutoff(int64_t now, int days);      /* now - (int64_t)days * 86400, 포화 뺄셈 */
int     trans_cache_cleanup_interval(int days);          /* clamp((int64_t)days*86400/10, 3600, INT_MAX - 3600) */
```
곱은 `int` 전체(음수 포함)에서 넘치지 않는다 (|days·86400| ≤ 1.86e14). 뺄셈은 `now` 가 극단값이면 넘치므로 (§8 R4)
결과를 `[INT64_MIN, INT64_MAX]` 로 포화시킨다. 모든 (int64 now, int days) 에서 정의된다.
네 곳(텍스트/SQLite × cleanup/stats)은 `(int64_t)entry->last_used < cutoff` 또는 `bind_int64(cutoff)` 로 바꾼다.
stats 의 `days <= 0` 동작(현재: 임계 = now 또는 미래)은 그대로 둔다 (호출자 없음 — `trans_cache_stats` 는 사용처 0).
주기 상한 `INT_MAX - 3600` 은 `elapsed += 5` 가 넘치지 않게 하는 값이다 (리셋 전 최대 상한+4).
cleanup 이 꺼져 있으면 `elapsed` 가 리셋되지 않으므로 (`http_server.c:98` 무조건 증가, 리셋은 `:108` 분기 안뿐 — §8 R5)
`elapsed` 는 cleanup 이 켜져 있을 때만 증가시킨다.

A2. 설정 `TRANS_CACHE_CLEANUP_DAYS`: `atoi` 와 **정의된 입력에서는 같은 값**을 내되 미정의 구간만 바꾼다.
- 앞 공백, 부호, 숫자열, 나머지 무시 — `atoi` 와 같다. 범위 밖은 미정의 대신 포화(`INT_MAX`/`INT_MIN`).
- 그 뒤 기존 규칙 `<= 0 → 60` 유지.
- 숫자 뒤 잔여 문자, 포화, 숫자 없음이면 `Warning:` 로그. 값 자체는 위 규칙을 따른다.
- 이유: 엄격 파싱 + 기본값 60 으로 떨어뜨리면 `120days` 처럼 지금 120 으로 동작하는 설정이 60 이 되어 **더 많이 삭제**된다. 포화는 "아주 큼" 이라는 의도를 보존하고 아무것도 지우지 않는다.
- 기동 시 실효값을 남기도록 백그라운드 스레드 시작 로그를 `LOG_DEBUG` → `LOG_INFO` 로 올린다 (시험이 실효값을 관측할 수 있게).

A3. `cache_tool cleanup <days>`: 파괴적 명시 명령이므로 엄격하게. 10진 숫자만, 1..INT_MAX, 아니면 오류·종료코드 1·아무것도 지우지 않음.
`30days`, `+30`, ` 30`, `99999999999` 는 거부된다 (지금은 30/30/30/전체 삭제).

A4. (Q1 승인 시) `cache_tool delete <id>`: 같은 엄격 파서, 1..INT_MAX.

## 4. 설계 B — NUL 디코딩 차단

B1. `strict_json.c` 에 문법 무관 검출기 추가:
```c
/* true when cJSON 1.7.15 would decode some \u escape in data to U+0000. */
bool json_decodes_nul(const unsigned char *data, size_t len);
```
규칙 (python 명세 `tests/cjson_nul_ref.py` 가 기준):
1. `len` 또는 첫 원시 NUL 바이트에서 멈춘다 (cJSON_Parse 가 보는 범위와 같다).
2. 왼쪽부터 `\` 와 다음 1바이트를 짝으로 묶는다. 다음 바이트가 `u` 이면 뒤 4바이트를 본다:
   4바이트가 모두 `[0-9A-Fa-f]` 이고 값이 0 이 아니면 6바이트를 건너뛴다. 그 외(값 0, 16진 아님, 4바이트 미만) → **true**.
3. 문자열 경계를 추적하지 않는다. 근거: cJSON 이 받아들이는 루트 값 안에서 `\` 는 문자열 안에만 있고,
   cJSON 의 문자열 끝 탐색도 `\` + 1바이트 짝짓기다. 16진 4자리에는 `\`·`"` 가 없으므로, 처음으로 NUL 을 만드는
   `\u` 이전까지는 두 짝짓기가 같다. 따라서 **cJSON 이 받아들인 범위 안에서는 정확히 일치**하고,
   루트 뒤 잔여물이나 cJSON 이 어차피 거부할 입력에서만 보수적으로 true 가 될 수 있다.
4. 서로게이트는 따지지 않는다: 짝이 아닌 서로게이트는 cJSON 이 파싱을 실패시키고, 짝은 두 `\u` 모두 0 이 아니다.

B2. 적용 방식 (§8 R1 로 수정): 검사는 **cJSON 이 실제로 소비한 바이트에만** 한다.
`cJSON_Parse(x)` 를 `cJSON_ParseWithOpts(x, &end, 0)` 로 바꾸고 (1.7.15 에서 `cJSON_Parse` 는 이 호출과 같은 동작이어야 한다 — V1·기존 시험으로 확인),
파싱이 성공하면 `json_decodes_nul(x, end - x)` 를 본다. V1 실측상 이 범위에서는 정확히 일치하므로
보수적 거짓 양성(루트 뒤 잔여물, SSE 주석·무시되는 이벤트)이 생기지 않는다.

| 지점 | 판정 시 | 상태 |
|---|---|---|
| `json_handler.c:parse_translation_request` | NULL 반환 | 422 VALIDATION_ERROR (Q2) |
| `mcp_client.c:wait_for_id` 의 일치한 줄 → `mcp_call_tool` | 오류 "codex MCP response contains U+0000" | 502, 재시도 없음, 캐시 없음 |
| `http_client.c:handle_non_streaming_response` | 오류 "Upstream response contains U+0000" | 502 (normal 은 `retryable ? 503 : 502`, `http_server.c:310-311`), 재시도 없음 |
| `http_client.c:parse_sse_chunk` (각 `data:` 페이로드) | 청크가 오류를 알리고 `handle_streaming_response` 가 전체를 실패시킨다 (그 청크만 건너뛰면 중간이 빠진 번역이 된다) | 위와 같음 |
| `http_client.c` codex 비구조화 결과: `memchr(result, 0, result_len)` | 오류 "codex output contains a NUL byte" | 502 |

MCP 의 `initialize` 응답에는 적용하지 않는다 (번역문이 아니며, 적용하면 지금 되는 초기화가 실패할 수 있다).
`wait_for_id` 에 `bool *nul_out` 를 더한다 (§8 R7): 함수 시작에서 false, 각 줄을 `cJSON_ParseWithOpts(start, &end, 0)` 로 파싱하고,
id 가 일치한 분기 안에서 **`memmove` 전에** `json_decodes_nul(start, end - start)` 를 계산한다 (`*nl = '\0'` 이므로 `end` 는 줄을 넘지 않는다).
`mcp_call_tool` 은 최상위 `error`·`isError`·내용 추출보다 먼저 이 값을 본다. `mcp_initialize` 는 NULL 을 넘긴다.

normal 백엔드 전파 (§8 R8): 두 응답 처리 함수에 `bool *nul_out` 를 더해 `translate_via_openai` 까지 올리고,
일반 `!raw_translation` 분기("No translation in response")보다 **먼저** 처리한다. `parse_sse_chunk` 의 기존 NULL 반환
(`data: ` 없음, `[DONE]`, 길이 0, 할당 실패, 파싱 실패, content 없음)은 지금처럼 무시하고, 파싱 성공 후 NUL 판정일 때만
`*nul_out = true` 로 응답 전체를 실패시킨다. 검사는 `json_str` 를 해제하기 전에 한다. 스트림 누적 버퍼는 거부 시 해제한다.

## 5. 검증

V1. **명세 대 실제 cJSON (정답 비교).** ctypes 로 `libcjson.so.1.7.15` 에 `cJSON_InitHooks` 로 할당자를 걸고,
할당 버퍼를 0xFF 로 채워 두고 실제 크기를 기록한다. 노드(64바이트, cJSON 이 0 으로 채움)도 같은 할당자를 쓰므로 (§8 R2)
**파싱된 트리를 순회해 `valuestring`·`string` 포인터가 가리키는 할당만** 본다. 크기로 구분하지 않는다.
종단은 끝의 0xFF 를 걷어낸 마지막 바이트(0x00 이어야 함 — 아니면 시험 실패)다. `parse_string` 은 `원시길이 - 이스케이프수 + 1` 바이트를 할당하고 디코드 결과와 종단 NUL 을 쓰므로,
"종단 NUL 앞에 0x00 이 있는가" 가 실제 잘림 여부다 (퍼즈 입력의 문자열에는 원시 0xFF 를 넣지 않는다).
`cJSON_ParseWithOpts` 의 `return_parse_end` 로 cJSON 이 소비한 범위를 얻어 **`spec(소비 범위) == 실제 잘림` 이 모든 수락 입력에서 성립**해야 한다.
전체 입력 기준으로는 거짓 음성 0 이어야 하고, 거짓 양성은 전부 소비 범위 밖에서 생긴 것이어야 한다 (개수 보고).
계획 단계 사전 측정 (스크래치, 2 시드 × 200,000): 수락 74,520/75,062 건에서 거짓 음성 0, 소비 범위 불일치 0.
전체 입력 기준 거짓 양성 7,431/7,401 건은 전부 소비 범위 밖이었다 (→ B2 에서 소비 범위만 검사).
V2. **C 대 명세 차분 시험**: `tests/test_cjson_nul.c` (길이 접두 레코드, test_strict_json 과 같은 방식) + `tests/test_cjson_nul.py`.
고정 사례(§1.2 의 전부 + `\\u0000`, `\\\u0000`, 대문자 16진, 서로게이트 짝, 끝의 `\`, `\u` 뒤 원시 NUL, 길이 경계) + 퍼즈 ≥ 200,000.
V3. **돌연변이**: 6 대신 7바이트 건너뛰기 (2바이트는 등가 돌연변이 — §8 R3), NUL 에서 멈추지 않기, 대문자 16진 거부, 짧은 꼬리를 false, 값 0 허용 — 각각 V2 가 잡아야 한다.
V4. **서버 종단** `tests/test_nul_guard.py` (`make test-nul-guard`): §1.2 의 표 전부가 새 상태가 되고 캐시 0행·업스트림 미호출(요청 거부)임을 확인.
대조군: `\\u0000`(문자 그대로) / `\u00e9` / 서로게이트 짝 / 일반 한글 / 루트 뒤 `\u0000` 잔여물 / SSE 주석의 `\u0000`(§8 R1) 은
지금과 같은 200·같은 바이트.
스트림 순서 시험 (§8 R8): `정상 → NUL → 정상` 은 502·캐시 0행, `정상 → 깨진 JSON → 정상` 은 지금처럼 200 (깨진 청크 무시).
V5. **cleanup**: `tests/test_cleanup_days.c` 하네스(두 백엔드에 지정 `last_used` 행을 넣고 `trans_cache_cleanup`/`_stats` 실행) +
`tests/test_cleanup_days.py` 가 python 으로 기대값 계산. days ∈ {1, 30, 24855, 24856, 36500, 49710, 49711, 60000, 20762, 20763, INT_MAX, 0, -1}.
경계는 now 의존이므로 행의 나이를 임계에서 ±1시간 이상 떨어뜨린다. 주기 함수는 python 으로 계산한 기대값과 전 범위 표본 비교.
`cache_tool cleanup/delete` 인자 표, 설정값 표(`60`, `120days`, `99999999999`, `-5`, `abc`, ` 30`) → 기동 로그의 실효값.
V6. **회귀**: 기존 전 시험(test-auto, coalescing, source-hint 계열, strict-json, structured-hint, cache-tool-hints, config limits),
`test_prompt_regression.py` 이전 바이너리 대비 동일, 새 하네스 ASan/UBSan, 서버 valgrind 0 오류. 경고 0 빌드.
V7. 실제 codex(exec) 로 정상 번역 1회 — 새 검사가 정상 출력에 영향이 없음을 확인 (MCP 는 설치된 0.166.0 에 없어 가짜로만 시험 가능함을 명시).

## 6. 순서와 커밋

0. 이 계획 + codex 검토 결과 커밋.
1. A (cleanup) + 시험.
2. B1 검출기 + 명세 + V1–V3.
3. B2 적용 지점 + V4.
4. 문서 (README 한계표, CLAUDE.md, INSTALL, TEST_RESULTS, PLAN_SOURCE_HINT_METADATA.md §14 상태 갱신).
5. 전체 회귀 → main fast-forward 병합 → 운영 디렉터리 재빌드. push 하지 않는다. 서비스 재시작하지 않는다.

## 7. 사용자 결정

- Q1. `cache_tool delete <id>` 도 고칠까 (권장: 예 — 같은 파서, 파괴적 오동작 실측).
- Q2. 요청에서 NUL 로 디코드되는 `\u` 를 422 로 거부할까 (권장: 예 — 지금은 다른 원문을 번역해 200 으로 돌려준다.
  `\u00ZZ` 류는 RFC 8259 위반 JSON 이라 문서화된 "JSON 파싱 실패 → 422" 와도 맞다).
- Q3. 설정 파싱을 "`atoi` 호환 + 포화" 로 할까, 엄격 거부(기동 실패)로 할까 (권장: 호환 + 포화 + 경고).

**사용자 결정 (2026-11-06)**: Q1 예 (delete 도 엄격 파서), Q2 예 (요청은 422), Q3 `atoi` 호환 + 포화 + 경고. 모두 권장안.

## 8. codex 교차검토 (1차, 계획 단계)

codex(read-only)에 계획과 인용 소스를 주고 적대적 검토를 맡겼다. 6건 모두 원문을 열고 실측해 판정했다.

| # | codex 주장 | 내 검증 | 판정 |
|---|---|---|---|
| R1 | 응답 본문 전체 검사는 무시되던 SSE 주석의 `\u0000` 때문에 정상 응답을 502 로 만든다 | `http_client.c:255` `strstr(chunk_start, "data: ")` 확인. 현재 바이너리에 `: diagnostic \u0000` + `data: {…"content":"OK"…}` 스트림을 주니 **200 `OK`** (`probe_r1.py`) | ✅ 채택 — B2 를 "소비 범위만 검사 + 청크별 오류 전파" 로 변경. 루트 뒤 잔여물의 보수적 거부(내 원안)도 함께 없어진다 |
| R2 | V1 할당자에는 노드 할당(64바이트, 0 으로 채워짐)도 잡히므로 문자열 할당만 골라야 한다 | ctypes 실측: `null` → 할당 [64], `"x"` → [3, 64], valuestring 바이트 `78 00 ff`. ctypes 구조체 크기 64 | ✅ 채택 — V1 에 선택 규칙 명시 (스크래치 하네스는 이미 트리 순회 방식이었다) |
| R3 | "6 대신 2바이트" 돌연변이는 등가라 잡을 수 없다 | 200,000건 퍼즈에서 2바이트 변이 차이 **0건**, 7바이트 변이 6,501건. codex 예시 `"\u0041\u0000"` 에서 명세 true, 7바이트 변이 false | ✅ 채택 — 7바이트로 교체 |
| R4 | `now - days*86400` 의 뺄셈은 `now` 극단값에서 넘친다 | python: `INT64_MIN - 86400 < INT64_MIN`, `INT64_MAX + 86400 > INT64_MAX` 참 | ✅ 채택 — 포화 뺄셈 |
| R5 | cleanup 이 꺼져 있으면 `elapsed` 가 리셋되지 않아 넘친다 | `http_server.c:98` 무조건 `elapsed += save_interval`, 리셋은 `:108-109` 의 `if (cleanup_enabled && …)` 안뿐. python: 429,496,730 번째 반복에서 2,147,483,650 (약 68.0년) | ✅ 채택 — 켜져 있을 때만 증가 (실효 영향은 없지만 미정의 동작 제거) |
| R6 | 완결된 루트 뒤의 원시 NUL 은 파싱 실패가 아니다 | ctypes: `{"id":2,…"content":"A"}}}` + `\x00junk` 수락, 60바이트 중 55 소비, 잘림 없음. `mcp_client.c:191` `cJSON_Parse(start)` 확인 | ✅ 채택 — §2 제외 항목 문구 정정 (동작 변경 없음) |

### 2차 (수정된 B2 대상)

| # | codex 주장 | 내 검증 | 판정 |
|---|---|---|---|
| R7 | MCP 서술이 아직 줄 전체(`nl - start`)를 가리키고, `memmove` 뒤에 검사하면 다른 바이트를 본다 | 계획 133행 확인. `mcp_client.c:202` 의 `memmove` 확인. ctypes: `{…"content":"OK"}}} \u0000` 은 수락, 63바이트 중 56 소비, 줄 전체 기준 명세 true · 소비 범위 기준 false | ✅ 채택 — 순서 명시 |
| R8 | `handle_streaming_response` 실패는 일반 "No translation in response" 분기로 가므로 NUL 전용 전파가 한 단계 더 필요 | `http_client.c` 의 `if (!raw_translation)` 분기("No translation in response", `status_code = http_code`) 확인. `parse_sse_chunk` 의 NULL 반환 경로 6종과 `cJSON_Parse` 직후 `free(json_str)` 를 열어 확인 | ✅ 채택 — 전파 경로와 무시 경로 구분 명시 |
| (확인) | `cJSON_Parse(x)` ≡ `cJSON_ParseWithOpts(x, &end, 0)`, `end` 는 루트 직후 | ctypes 100,000건: 결과(재직렬화)·오류 위치 차이 0건 (수락 37,369) | ✅ B2 전제 확인 |
| (확인) | glibc `atoi` = `(int)strtol`; strtol 결과를 int 범위로 포화하면 정의된 입력에서 같다 | ctypes 199,999건(정의된 입력) 불일치 0. 참고: 범위 밖에서 glibc `atoi` 는 `-2147483649 → 2147483647`, 30자리 9 → `-1`(→ 지금은 60일) | ✅ A2 전제 확인 |
| (확인) | 설정 줄은 줄 전체만 공백·탭·CR·LF 로 다듬고 값은 다듬지 않는다 | `config_loader.c:27-38` 다듬기, `:84-86` 값 복사 확인 | ✅ 시험 표에 VT/FF·앞 공백 사례 포함 |
| (확인) | `cache_tool cleanup` 은 텍스트 백엔드만 | `cache_tool.c:1029` `trans_cache_init`, `trans_cache.c:80-82` → `CACHE_BACKEND_TEXT` 확인 | ✅ V5 그대로 |
| (확인) | 일 단위 곱은 다섯 곳뿐 | `grep -nE '\* *(24|60|86400|3600|1000)\b|86400' src/*.c` → 5곳 + `(long)` 승격된 ms 계산들 | ✅ |

수렴: 1차 6건(설계 결함 1) → 2차 2건(서술 누락, 설계 반례 없음). 구현 diff 는 병합 전에 다시 codex 로 검토한다.

내 쪽 오류: R1 은 내 원안의 결함이었고, R6 은 내 서술이 틀렸다. 검증 중 내 검증 스크립트의 상수 가림 버그로
R3 수치를 한 번 잘못 얻었다가 (명세가 항상 false) 고쳐 다시 측정했다.

### 3차 (구현 diff, 병합 전)

`git diff 01569a6..HEAD -- src include` 를 맡겼다. 결함 지적 0건. codex 가 덧붙인 사실 주장은 직접 확인했다:
옛 `24 * 60 * 60` 곱 0곳(grep), `wait_for_id` 호출자 2곳 모두 갱신, 남은 `cJSON_Parse` 는 텍스트 캐시 적재와
기동 시 `mcp list --json` 확인 2곳(둘 다 §2 제외 범위). "결함 없음" 은 근거가 아니므로 valgrind·회귀로 따로 확인했다.

## 9. 구현 결과 (2026-11-06)

커밋: `8e2e162` (A), `6bce898` (B1), `754cc8f` (B2), 문서. 수치는 [TEST_RESULTS.md](TEST_RESULTS.md).

계획과 다르게 된 것:
1. 돌연변이 시험에서 `\u` 대신 `\U` 를 받는 변이가 처음에 살아남았다. 시험 입력에 대문자 `U` 가 없었기 때문이다.
   고정 사례 2건과 생성기 토큰 2종을 더해 잡았다 (7/7).
2. 검증 스크립트 실수 2건(상수 가림으로 명세가 항상 false, 문자열 분할로 인자 개수를 8로 셈)은 산출물에 들어가기 전에
   발견해 다시 측정했다.
3. 도구 입력이 `\u00e9` 같은 이스케이프를 실제 문자로 바꿔 쓰는 일이 있었다 (이 문서 초안의 한 줄). 이후 모든 파일을
   자리표시자로 쓰고 기록 후 검사했다.

남은 것: MCP 경로는 실제 서버로 시험할 수 없다 (codex-cli 0.154.0 에서 제거). 텍스트 캐시 적재와 `mcp list` 확인은 의도적으로 제외했다.
