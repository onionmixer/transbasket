# 번역 백엔드 모듈 분리 계획 (normal / codex, codex 실행 방식 교체 지점)

작성 2026-11-06. 작업 위치: worktree `../transbasket-backend`, 브랜치 `refactor/backend-modules` (main `cfde092` 위, 문서 정정 `60a225e` 다음).
원칙: 동작은 바이트 단위로 그대로 (프롬프트·요청 본문·응답·상태 코드·로그 문구와 순서), 계산·집계는 python,
codex 는 입력일 뿐 (지적은 전부 코드·실측으로 확인 후 채택), push·서비스 재시작은 요청 시에만.
이 문서의 `파일:줄` 은 main `cfde092` (레거시 MCP 제거 반영) 기준이다. 상태: **계획, codex 계획 검토 1회 반영 (§8), 코딩 전.**

## 1. 현재 구조 (실측)

`src/http_client.c` 1,559줄에 두 백엔드가 함께 있다. 함수 26개를 python 으로 호출 관계까지 뽑아 분류했다:

| 묶음 | 함수 | 함수 줄 수 |
|---|---|---|
| 공통 — 프롬프트 | `str_replace` `is_translategemma_format` `build_auto_instruction` `build_instruction_message` | 220 |
| 공통 — trace 파일 | `open_trace_file` (함수 안 `static atomic_bool warned`, 경고 1회) `trace_base` | 41 |
| normal 전용 | `translation_progress` `transbasket_curl_write_callback` `save_debug_curl` `sse_status_message` `sse_event` `sse_field` `handle_streaming_response` `handle_non_streaming_response` `translate_via_openai` | 633 |
| codex 전용 | `trim_inplace` `unwrap_full_fence` `make_delimiter` `save_debug_codex` `write_schema_file` `parse_structured_output` `translate_via_codex` | 353 |
| 바깥 API·선택 | `openai_translator_init` (codex 블록 `:661-714`: 세마포어, CLI 탐지, 경고 5종 — `:686,689,692,700,707`; `--ephemeral` 없음은 경고 없음) `openai_translator_free` `openai_translate` (백엔드 표 `:1516`, 전체 마감 → 503/504) `free_translated_text` | 135 |

- `http_client.h` 가 선언하는 함수는 4개 (`openai_translator_init/free`, `openai_translate`, `free_translated_text`), 바깥에서 부르는 것은 앞의 3개와 `TranslationError` 뿐이다
  (`http_server.c:305,307,452,468,679`; `free_translated_text` 는 바깥 호출 0건). `OpenAITranslator` **타입**은 `http_server.h:17` 이 포인터로 쓴다.
  `OpenAITranslator` 의 필드를 `http_client.c` 밖에서 읽는 곳은 없다 (grep 0건). `OpenAITranslator` 에 codex 전용 필드 4개가 있다 (`http_client.h`).
- codex CLI 실행은 이미 `codex_exec.c` (536줄) 로 나뉘어 있다: `CodexExecOptions` + `codex_exec_run()` + `codex_cli_probe()`.
  그러나 **exec 에만 해당하는 판단이 `http_client.c` 에 있다**: 탐지 결과 해석과 경고 (`--ephemeral`, `--ignore-user-config`, `--output-schema`,
  config.toml MCP 서버), `CodexExecOptions` 조립, structured 가능 여부 (`codex_output_schema`).
  MCP 제거 때 드라이버 (`mcp_client.c`) 가 따로 있었는데도 `http_client.c` 세 곳을 고쳐야 했던 이유다.

## 2. 목표

1. 백엔드별 파일 분리 — normal 을 고칠 때 codex 를, codex 를 고칠 때 normal 을 열 필요가 없게.
2. codex 의 **실행 방식 (transport)** 과 **번역 절차** 를 분리 — 실행 방식이 바뀌면 (새 CLI 하위 명령, app-server, SDK 등)
   드라이버 파일·표·설정 저장·기동 로그 4곳 (§4) 으로 끝나고, 슬롯·마감·임시 cwd·구분자 격리·structured 검증·후처리·상태 코드 규칙은 재사용.
3. 동작 변화 0. 바깥 API (`http_client.h` 의 함수 4개, `TranslationError`, `OpenAITranslator` 타입 이름) 이름·시그니처 유지 → `http_server.c`, `main.c` 무수정.

하지 않는 것: 이름 바꾸기 (`openai_*`, `OpenAITranslator`), 동작 수정, normal 의 재시도 구조 변경, 두 번째 드라이버 구현.

## 3. 단계 1 — 파일 분리 (코드 이동만)

| 새 파일 | 내용 | 바깥에 내놓는 것 (`static` 해제) |
|---|---|---|
| `src/prompt_builder.c` + `include/prompt_builder.h` | 공통 — 프롬프트 4함수 | `build_instruction_message`, `str_replace` (codex 가 `{TEXT}` 치환에 씀) |
| `src/trace_file.c` + `include/trace_file.h` | `open_trace_file`, `trace_base` | 둘 다 |
| `src/backend_normal.c` + `include/backend_normal.h` | normal 전용 9함수, `CurlResponse`, `SseStatus`, `MAX_UPSTREAM_RESPONSE` | `translate_via_openai`, `normal_backend_init` (init 의 else 블록) |
| `src/backend_codex.c` + `include/backend_codex.h` | codex 전용 7함수, `SOURCE_HINT_SCHEMA_JSON`, init 의 codex 블록 → `codex_backend_init`, free 의 `sem_destroy` → `codex_backend_free` | `translate_via_codex`, `codex_backend_init`, `codex_backend_free` |
| `src/http_client.c` (남김) | `openai_translator_init/free`, 백엔드 표, `openai_translate`, `free_translated_text`, `DEFAULT_*`, `curl_global_init/cleanup` | (기존 그대로) |

- 내놓는 이름은 9개: 지금 `static` 인 6개 (`build_instruction_message` `str_replace` `open_trace_file` `trace_base` `translate_via_openai` `translate_via_codex`)
  — `src/ include/ tests/*.c` grep 에서 전부 `http_client.c` 에만 있다 — 와 새로 만드는 3개 (`normal_backend_init` `codex_backend_init` `codex_backend_free`, 현재 0건).
- 함수 밖 항목도 모두 자리를 정한다: `#include` 목록 (`:6-29`, 각 새 파일은 자기가 쓰는 것만, 단 `backend_normal.c` 는 `test_sse.c` 가 include 하므로
  옛 파일의 순서를 유지), `DEFAULT_TIMEOUT/DEFAULT_MAX_RETRIES` (`:31-32`, facade), `MAX_UPSTREAM_RESPONSE`·`CurlResponse` (`:34,37`, normal),
  `SseStatus` (`:201`, normal), `SOURCE_HINT_SCHEMA_JSON` (`:1191`, codex), `TranslateFn`·`TranslationBackend`·표 (`:1506-1519`, facade),
  시험 훅 블록 2개 (`write_schema_file` 안 `:1206`, `translate_via_codex` 안 `:1341`) 는 함수와 함께.
- init 순서·로그 순서 보존: 할당·기본값 → `curl_global_init` (두 모드 모두) → codex 는 세마포어 → 탐지 → 경고 → `codex translator initialized`.
- **실패 정리의 주인은 facade 하나**: 지금 세마포어 실패는 그 자리에서 `curl_global_cleanup` + `free(translator)` 를 한다 (`:663-671`).
  `codex_backend_init` 은 실패 시 자기 것만 되돌리고 -1 을 돌려주며, curl 정리와 `free` 는 facade 가 한다 (이중 정리 방지).
  해제 순서는 지금대로 세마포어 (`codex_sem_inited` 가드 유지) → `free` → `curl_global_cleanup` (`:727-739`).
- **요청 처리 순서 보존** (`translate_via_codex`): 슬롯 반환 → 스키마 파일 삭제 → 임시 cwd 삭제 → DEBUG trace (원시 결과 포함) → 실패 보고 또는
  NUL 검사·structured 검증·후처리 (`:1413-1501`). 그래서 structured 거부는 trace 가 거부 로그보다 먼저 남는다 — 이 순서를 바꾸지 않는다.
- `OpenAITranslator` 의 codex 필드는 단계 1 에서 그대로 둔다 (구조체 변경은 단계 2 와 함께).
- 테스트 영향 (전수 grep, 테스트 타깃은 `test-*` 규칙 22개 — `test-inflight-unit` 은 `test-coalescing` 이 먼저 돌린다, `Makefile:102`):
  - `tests/test_sse.c:13` 가 `#include "../src/http_client.c"` (static 접근 + `#define malloc` 로 그 파일 안의 k 번째 malloc 실패 주입).
    → `backend_normal.c` 를 include 하도록 바꾸고, `Makefile:163` 의 `filter-out` 과 `tests/test_sse.py:88,90,99` 의 파일 이름 (돌연변이 빌드의 제외 목록 포함) 을 같이 바꾼다.
    facade·공통 모듈은 그대로 링크된다 (훅은 include 된 파일 안의 malloc 만 센다).
    `handle_streaming_response` 가 부르는 함수 (`sse_event`, `sse_field`) 는 같은 파일로 가므로 malloc 번호 매김은 그대로여야 한다 — 확인 항목 V7.
  - 그 밖에 `http_client` 를 이름으로 참조하는 테스트·빌드 규칙 없음 (grep). 서버 빌드는 `$(wildcard src/*.c)` (`Makefile:22`) 라 새 파일이 자동 포함.
    `CACHE_TOOL_SRCS` (`Makefile:26`) 는 명시 목록이고 이 파일들과 무관.

### 3.1 새 헤더의 선언 (지금 정의에서 `static` 만 뺀 것, 이름·인자 그대로)

```c
/* include/prompt_builder.h  (includes http_client.h for OpenAITranslator) */
char *str_replace(const char *orig, const char *rep, const char *with);
char *build_instruction_message(OpenAITranslator *translator,
                                const char *from_lang, const char *to_lang,
                                const char *text, bool *is_single_message, bool structured);
/* include/trace_file.h */
FILE *open_trace_file(const char *base, const char *ext, char *path, size_t path_size);
void  trace_base(char *out, size_t size, const char *timestamp, const char *uuid);
/* include/backend_normal.h, include/backend_codex.h: translate_via_openai / translate_via_codex
 * with the TranslateFn signature (http_client.c:1506), plus
 *   void normal_backend_init(const Config *config);                 -- the else-branch logs
 *   int  codex_backend_init(OpenAITranslator *t, const Config *config, int max_concurrency); -- 0 / -1
 *   void codex_backend_free(OpenAITranslator *t);                    -- sem_destroy if inited */
```

- 결정 N1: 이름을 바꾸지 않는다 (`str_replace` `trace_base` 처럼 흔한 이름이 전역 심볼이 된다). 링크하는 라이브러리 (curl, microhttpd, cjson,
  uuid, ssl, crypto, sqlite3, m) 의 동적 심볼에 같은 이름이 없는지 `nm -D` 로 확인한다 (V2). 겹치면 그때 `tb_` 접두사.
- 결정 N2: `static` 을 뺄 때 이어지는 줄의 들여쓰기는 건드리지 않는다 (V1 이 "첫 줄의 `static ` 만 다름" 으로 확인할 수 있게).

## 4. 단계 2 — codex 드라이버 교체 지점

`backend_codex.c` 가 드라이버 표에서 `CODEX_INTERFACE` 값으로 드라이버를 고른다. 지금 행은 `exec` 하나.

```c
/* include/codex_driver.h */
typedef struct {
    bool structured_output;          /* can return output for a JSON schema (structured hint mode) */
} CodexDriverCaps;

typedef struct {                     /* what the backend decided; keeps today's filesystem workflow (cwd, schema file) */
    const char *prompt;
    const char *cwd;                 /* per-request temp dir, NULL = none */
    const char *schema_path;         /* NULL = plain output */
    const TranslationControl *control;
} CodexRunRequest;

typedef struct {
    const char *name;                /* CODEX_INTERFACE value */
    /* startup: probe the CLI, log the driver's own warnings, fill caps. A missing or old CLI only
     * warns, as today; -1 only when the driver's state cannot be allocated (see §4 결정 S1). */
    int   (*init)(const Config *cfg, void **state, CodexDriverCaps *caps);
    void  (*free)(void *state);
    /* one translation; same contract as codex_exec_run(): NULL + *err_out, *timed_out on deadline,
     * *stderr_out only when the caller passes it (DEBUG). */
    char *(*run)(void *state, const Config *cfg, const CodexRunRequest *req, size_t *len_out,
                 char **err_out, bool *timed_out, char **stderr_out);
} CodexDriver;
```

- **계약** (지금 `codex_exec_run()` 이 지키는 것 그대로, `codex_exec.h:52-58`): 결과는 호출자가 `free` 하는 쓰기 가능한 버퍼 (후처리가 제자리에서 고침),
  `*len_out` 은 붙인 종단 NUL 을 빼고 **안에 든 NUL 바이트는 포함** (backend 의 `memchr` 검사가 이 길이를 쓴다), `*err_out` 은 호출자가 `free` 하고
  클라이언트에 보여도 되는 짧은 문구 (프롬프트를 담지 않음), `*stderr_out` 은 DEBUG 일 때만, 호출자가 `free`, 길이 제한. 요청의 포인터는 호출 동안만 쓴다.
  `require_output_file` 은 `schema_path != NULL` 에서 나온다 (지금 둘 다 `structured`).
- **결정 S1**: exec 드라이버 상태 (`has_ephemeral` 등) 는 init 에서 할당한다. 할당 실패는 기동 실패 (`Error: Memory allocation failed`) —
  지금 `calloc(OpenAITranslator)` 실패와 같은 부류이고, 그 밖의 경우 (CLI 없음·옛 CLI) 는 지금처럼 경고만. 메모리 부족에서만 생기는 새 실패 지점이라 차이로 기록한다.
- **exec 드라이버** = 새 어댑터 `src/codex_driver_exec.c` (`const CodexDriver codex_exec_driver`). `codex_exec.c` 는 `Config` 를 모르는 지금 모습 그대로 둔다
  (`CodexExecOptions` + `codex_exec_run/codex_cli_probe`). 어댑터만 `Config` 와 `codex_exec.h` 를 둘 다 안다:
  init = 지금 `openai_translator_init` 의 탐지와 `codex CLI: ... (interface=exec)` 줄, 경고 5종 (`:686,689,692,700,707`) 을 **같은 순서·같은 문구**로, 그리고 `has_ephemeral` 보관.
  backend init 은 세마포어 → `driver->init` → `codex translator initialized` 순 — 지금 로그 순서와 같다.
  run = `CodexRunRequest` + `Config` → `CodexExecOptions` 조립 (지금 `http_client.c` 의 조립 그대로) → `codex_exec_run`.
- **backend 에 남는 것** (드라이버와 무관): 세마포어 슬롯과 대기 마감, 임시 cwd, 스키마 파일 쓰기·삭제, structured 결정 (`caps.structured_output` 사용),
  프롬프트 조립·구분자, 결과 후처리 (NUL 검사, trim, fence, structured 파싱), 상태 코드 규칙 (실패 502, 마감 504 + retryable), DEBUG trace.
  `codex CLI: ... (interface=exec)` 와 `codex translator initialized` 로그 줄은 어느 쪽이 찍든 문구·순서 그대로.
- `OpenAITranslator` 의 codex 필드 (`codex_sem`, `codex_sem_inited`, `codex_ephemeral`, `codex_output_schema`) 는
  `const CodexDriver *`, `void *driver_state`, `CodexDriverCaps`, 세마포어로 바뀐다 (`http_client.c` 밖에서 안 읽음 — §1).
- 설정: 단계 2 에서 `config_loader.c` 는 바꾸지 않는다 (`exec` 만 받음, `mcp` 는 제거 오류 그대로). 지금 `Config` 는 고른 인터페이스를 저장하지 않고
  (`codex_interface_mcp` 만, `config_loader.h:79`, 파서 `config_loader.c:716`) `main.c:187` 은 `exec` 를 고정 출력한다. 그래서 두 번째 드라이버를 넣을 때는
  **드라이버 파일 + 표 1줄 + `Config` 에 인터페이스 이름 저장 (파서·검증) + `main.c` 출력** 이 필요하다 — §2 의 "파일 1개 + 표 1줄 + 설정 값 1개" 는
  이 4곳으로 정정하고, CLAUDE.md 에 "드라이버 추가 절차" 로 적는다. 단계 2 에서 미리 `Config` 를 바꾸지는 않는다 (쓰는 곳이 없다).
- 지금의 숨은 결합을 드러내는 곳: structured 모드는 "exec 이고 `--output-schema` 를 광고" 였고 MCP 제거 후 "`--output-schema` 를 광고" — 단계 2 에서 `caps.structured_output` 로 같은 값.

## 5. 검증 (기준 바이너리 = MCP 제거가 합쳐진 main)

| # | 무엇 | 방법 |
|---|---|---|
| V1 | 단계 1 이 이동뿐인가 (도구: `tests/check_move.py <옛 파일> <새 파일...>`, 깊이 0 에서 빈 줄로 나뉜 덩어리 = 주석+정의 단위로 비교) | python: 옛 `http_client.c` 의 함수·typedef·매크로·정적 데이터·전처리 블록·dispatch 표를 하나씩 뽑아 새 파일들 중 **정확히 한 곳**에 글자 그대로 있는지 (include 는 집합으로 대조). 예외 (`static` 제거 9곳, init/free 분할) 는 목록으로 두고 줄 단위 diff 를 출력해 사람이 본다 |
| V2 | 심볼 | `nm -g --defined-only` 옛/새 오브젝트 합집합 비교: 옛 심볼 손실 0, 새 심볼은 §3·§4 목록과 일치 (존재만 본다 — 시그니처·동작은 V1·V3-V5 가 본다) |
| V3 | 프롬프트·요청 본문 | `tests/test_prompt_regression.py <기준> <새>` 24건 동일. **추가 V3b**: 지금 회귀 픽스처는 `--output-schema` 를 광고하지 않아 (`fake_codex_auto.py:31`) structured 프롬프트를 비교하지 않고, `test_structured_hint.py:151` 은 부분 문자열만 본다 → `--output-schema` 를 광고하는 픽스처로 structured 프롬프트·스키마 파일 바이트·**argv 전체** (`--ephemeral` `--ignore-user-config` `-m` `-s` `-c` 순서 포함) 를 옛/새 비교 |
| V4 | 기동 로그 (요청 로그도) | python: 같은 설정으로 옛/새 바이너리 기동, 시각·포트·pid 를 가린 로그 전체가 같은지. 설정 행렬: normal, normal+`SOURCE_HINT_MODE=model`, codex 가짜 CLI 의 능력 조합 (버전 실패 / exec 없음 / `--ephemeral` 없음 / `--ignore-user-config` 없음+설정 켬 / `--output-schema` 없음+model 모드 / config.toml MCP 서버 있음+격리 끔·켬). 같은 실행에서 요청 몇 개를 보내 **요청 처리 로그의 줄 순서**와 DEBUG trace 파일 내용 (stderr 꼬리 포함) 도 비교 |
| V5 | 응답 | 회귀 도구를 넓혀 옛/새의 HTTP 상태·본문·`Retry-After` 도 비교. 성공 응답은 요청 시각을 그대로 돌려주지만 **오류 응답은 현재 시각을 넣으므로** (`json_handler.c:237-239`) 오류 본문의 `timestamp` 만 가린다. 슬롯 초과 503 은 `CODEX_TIMEOUT` (슬롯 대기 마감, `http_client.c:1302-1316`) 을 `TRANSLATION_TOTAL_TIMEOUT` 보다 짧게 둬야 504 로 덮이지 않는다 (`:1541`). 경우: exec 성공·실패 (502)·마감 (504)·슬롯 초과 (503)·structured 위반 (502), normal 4xx (502)·5xx 재시도 후 503·SSE 오류 (502) |
| V6 | 기존 회귀 | 21개 타깃 + `test_config_limits.py`, ASan/UBSan 서버, valgrind |
| V7 | SSE 시험 | `test_sse.py` 의 차등 6만 건 불일치 0. malloc 실패 주입은 후보 바이너리에서 횟수를 세므로 그것만으로는 "같다" 가 안 된다 → 옛/새 하네스로 본문마다 malloc 횟수와 k 번째 실패 결과를 **둘 다 뽑아 python 으로 대조** (지금 합계 73). 돌연변이 9개 anchor 가 새 파일에서 유일, 8개는 잡히고 1개는 기존대로 등가 (`content type unchecked`) |
| V8 | 단계 2 교체 지점이 실제로 쓸 수 있나 (가짜 드라이버는 exec 어댑터를 우회하므로 어댑터는 V3b 의 argv 대조가 맡는다) | 신규 C 시험 (`TRANSBASKET_TEST_HOOKS` 빌드에서만 드라이버를 바꿔 끼움): 가짜 드라이버의 성공·오류·마감·err 없는 NULL·NUL 포함 결과마다 상태 코드·retryable, 슬롯 반환, 임시 cwd·스키마 파일 삭제, caps 에 따른 structured 켜짐/꺼짐, 드라이버가 받는 프롬프트가 exec 경로와 같은지 |
| V9 | 실제 (연기 시험일 뿐) | host codex (0.167.0) 로 pge4 복사본 `-d -l` exec 번역 kor/auto, 배포 설정 3개 기동 경고 0 |

커밋: ① 단계 1 (V1–V7) ② 단계 2 (V2–V8) ③ 문서·TEST_RESULTS (V9). 각 커밋에서 전체 회귀.

## 6. 위험과 대책

| 위험 | 대책 |
|---|---|
| 이동 중 글자 하나 바뀜 | V1 (python 글자 대조), V3/V4/V5 차등 |
| init 분할로 로그 순서·조건이 바뀜, 실패 시 이중 정리 | V4 설정 행렬, 정리 주인을 facade 로 하나 (§3), 세마포어 실패 경로는 V8 하네스에서 `sem_init` 실패 주입 |
| `test_sse.c` 의 `#define malloc` 이 include 하는 파일의 다른 헤더에도 걸림 | 지금도 같은 구조 (`stdlib.h` 를 먼저 include). 새 파일의 include 목록을 옛 파일과 같은 순서로 |
| 공통 trace 의 "경고 1회" 가 백엔드별로 갈라짐 | `open_trace_file` 은 한 파일·한 함수로만 둔다 (`static atomic_bool` 그대로) |
| 드라이버 인터페이스가 하나뿐인 구현에 맞춰 과설계 / 과소설계 | 필드는 지금 exec 가 실제로 받는 것만. 넓히는 것은 두 번째 드라이버가 생길 때 |
| `TRANSBASKET_TEST_HOOKS` 위치 (`translate_via_codex` 안 `getenv("TB_TEST_FAIL_PROMPT")`) | 함수와 함께 이동, `make test-structured-hint` 로 확인 |

## 7. 문서

CLAUDE.md (모듈 목록 `:73`, dispatch `:180`, 프롬프트 `:378`, 크기 표 `:423,426`, "Modifying OpenAI Retry Logic" `:505`, 새 "codex 드라이버 추가 절차"),
README (`:575`, 트리 `:701,707`, 모듈 설명 `:749`), TEST_RESULTS 새 절. `TODO.md`·옛 PLAN 문서는 이력이라 그대로.

## 8. codex 교차검토 판정 (계획 단계, 코딩 전)

| codex 주장 | 내 검증 방법 | 결과 |
|---|---|---|
| 함수 26개·묶음 4/2/9/7/4·줄 수 220/41/633/353/135 | 내 python 집계와 같음 (호출 관계 스크립트) | ✅ |
| 함수 밖 항목 (include·매크로·typedef·표·시험 훅 2곳) 이 계획에 없다 | `grep TRANSBASKET_TEST_HOOKS` → `:1206`, `:1341`; 파일 머리·`:1506-1519` 열람 | ✅ 채택 (§3, V1) |
| 세마포어 실패 시 그 자리에서 curl 정리+free, 해제 순서 세마포어→free→curl | `:661-671`, `:727-739` 열람 | ✅ 채택 (§3 정리 주인) |
| 요청 처리 순서: 슬롯→스키마→cwd→trace→실패/검증 | `:1413-1501` 에서 `sem_post` `unlink` `rmdir` `save_debug_codex` `memchr` `parse_structured_output` 순서 확인 | ✅ 채택 (§3, V4) |
| SSE include/filter 계획은 맞음, 돌연변이 제외 목록도 바꿔야 | `test_sse.py:90` 열람 | ✅ 채택 |
| 드라이버 계약 (소유권, NUL 포함 길이, init NULL 의미) 이 없다 | `codex_exec.h`, `:1429` 이후 `memchr(result,0,result_len)` 확인 | ✅ 채택 (§4 계약, 결정 S1) |
| "설정 값 1개" 는 틀림: `Config` 는 bool 만, `main.c` 는 exec 고정 | 내가 이번에 쓴 코드 (`config_loader.h`, `main.c:187`) 열람 | ✅ 채택 (§4 정정, 내 오류) |
| "transport-agnostic" 은 과장 (cwd·스키마 파일 경로) | 계획 문구 | ⚖️ 문구만 정정 |
| V3 는 structured 프롬프트를 비교하지 않음 | `fake_codex_auto.py:31` (`--output-schema` 없음), `test_structured_hint.py:151` (부분 문자열) | ✅ 채택 (V3b) |
| 오류 응답은 현재 시각 → V5 의 "시각 그대로" 는 틀림 | `json_handler.c:237-239` (`get_current_timestamp`), 성공은 `:192-193` 요청 값 | ✅ 채택 (V5 정정, 내 오류) |
| 슬롯 503 이 전체 마감 504 로 덮일 수 있음 | 슬롯 마감 = `codex_timeout` (`:1302-1316`), 전체 마감 덮어쓰기 `:1541` | ✅ 채택 (V5 조건) |
| V7: malloc 횟수는 후보에서 세므로 같음을 증명 못 함, 등가 돌연변이 1개 | `test_sse.py:52-64` (`alloc_failures`), `EQUIVALENT` 1개 | ✅ 채택 (V7) |
| flag 전달·DEBUG stderr·요청 로그 순서 미검증 | 픽스처가 argv 를 검사하지 않음 (`fake_codex_auto.py:34-41`) | ✅ 채택 (V3b, V4) |
| 경고 6종 → 5종 | python: codex 블록의 `Warning` 줄 5개 | ✅ 정정 (내 오류) |
| 내놓는 이름 8 → 9 (새 3개는 현재 0건) | 계획 §3 목록 셈 | ✅ 정정 (내 오류) |
| `test-*` 규칙 22개 | python: Makefile 정규식 → 22개, `test-inflight-unit` 은 `test-coalescing` 선행 (`Makefile:102`) | ✅ 정정 |
| 헤더 함수 4개, `OpenAITranslator` 타입은 `http_server.h` 가 씀 | python 헤더 추출 4개, `http_server.h:17` | ✅ 정정 (내 오류) |
