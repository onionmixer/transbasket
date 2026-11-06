# source_hint 메타데이터 작업계획 — `from=auto` 번역의 원문 기준언어 기록

작성일: 2026-11-06 (codex 교차검토 5회 반영본)
상태: **구현·검증 완료 (2026-11-06, §15).** 계획 확정 후 사용자 결정 반영(§13). codex 교차검토 5회의 지적은 전부 원문·실측으로 재검증했다. 판정표와 수렴 판단은 §12 에 있다.
기준 코드: 작업 트리. 동일 요청 병합(coalescing)이 **미커밋**이므로, 구현 착수 전에 먼저 커밋한다 (§10 단계 0).

## 0. 목적과 결론

번역본마다 **원문의 기준언어**가 필요하다. 명시적 언어 요청은 `from` 이 곧 기준언어이고, 문제는 `from=auto` 다.
지금 캐시에는 `auto` 만 남고, 원문이 실제로 무슨 언어였는지는 기록되지 않는다.

1. **캐시 키는 바꾸지 않는다.** `SHA-256("auto|to|text")` 와 `from_lang='auto'` 를 그대로 둔다 (§2).
2. 기준언어는 키가 아닌 **메타데이터 `source_hint`** 로 저장한다.
3. 산출 경로는 둘이다.
   - **모델 보고(주 경로):** codex exec `--output-schema` 로 번역문과 원문 언어를 한 번에 구조화해서 받는다 (§4).
   - **문자 체계 분류기 v1(보조 경로):** 정확히 고정한 규칙이고, python 참조 구현이 곧 명세다 (§5).
4. SQLite 는 **별도 테이블 + 삭제 트리거 + 뷰**에 저장하고 `user_version` 은 2 로 유지한다.
   구버전 바이너리로 롤백해도 캐시는 그대로 쓸 수 있다 (실측, §6).
5. 우선순위는 **번역 무결성 > hint** 다. hint 처리가 번역 응답을 망가뜨리거나 지연시켜서는 안 된다.

## 1. 현재 코드 사실 (이번 세션에서 직접 확인)

| 사실 | 근거 |
|---|---|
| 캐시 키는 `from|to|text` 의 SHA-256 | `src/trans_cache.c:trans_cache_calculate_hash()` |
| 저장되는 `source_text` 는 ANSI·제어문자를 제거한 요청 본문이고, 해시도 같은 문자열로 만든다 | `src/http_server.c:handle_translate()` |
| `auto` 는 명시적 언어 캐시와 공유하지 않고, 응답에 감지 언어를 넣지 않는다 (현행 계약) | `PLAN_SOURCE_LANGUAGE_AUTO.md` §4.1, §2.2 |
| **엔진 결과는 병합 owner 만 저장한다.** 저장은 `add` / `update_count` / `update_translation` 중 하나다. **캐시 적중 요청도 `update_count` 를 호출한다** | `http_server.c:297-305` (owner), `:325-326` (cache_hit) |
| 공개 캐시 API 는 각 함수 안에서 쓰기 잠금을 잡고 푼다. 바깥에서 같은 잠금으로 여러 호출을 묶을 수 없다 (rwlock 비재진입) | `trans_cache.c:129-132`, `:143-145`, `:158-160` |
| owner 결과는 마감 이후 게시되면 폐기된다 | `inflight_translation.c:131-135` |
| codex 경로에는 출력 지시가 **두 곳**에 있다: `build_auto_instruction()` 의 "Return ONLY the translated text, without language labels…" 와, `translate_via_codex()` 의 고정 constraint "Return ONLY the translated text - no explanations…" | `http_client.c:build_auto_instruction()`, `:1100-1103` |
| codex 프롬프트는 임시 cwd 생성(`:1138`)보다 **먼저** 만들어진다(`:1065`, `:1110`) | `http_client.c` |
| `codex_exec_run()` 은 `-o` 파일이 없거나 비면 stdout 으로 대체한다 | `codex_exec.c:514-517` |
| 이후 후처리는 C 문자열(`strlen`) 기반이다 | `http_client.c:980`, `:993`, `:1243` |
| codex 구분자는 요청마다 UUID 로 새로 만든다 | `http_client.c:make_delimiter()` `:1009-1023` |
| SQLite `apply_schema()` 는 `user_version > 2` 이면 실패한다. `known_cache_columns()` 는 정확히 9개 열을 요구한다 (열을 추가하면 구버전이 DB 를 거부) | `cache_backend_sqlite.c` |
| v0/v1→v2 이전 시 `trans_cache` 의 인덱스·트리거 SQL 을 저장했다가 테이블 교체 후 다시 실행한다 | `migrate_source_constraint()` `:142` |
| 텍스트 백엔드: 알 수 없는 JSON 필드는 무시하고 읽고, 저장은 고정 9개 필드. lookup 사본은 필드 단위 복사 | `cache_backend_text.c:load_cache_from_file()`, `text_backend_save()`, `text_entry_clone()` `:226-249` |
| 텍스트 `add()` 는 같은 해시가 있으면 성공(0)을 반환하고, SQLite `add()` 는 UNIQUE 위반으로 실패(-1) | `cache_backend_text.c:302-303`, `cache_backend_sqlite.c:499-501` |
| `cache_tool` 은 `migrate` 만 백엔드를 고르고, 나머지 명령은 `trans_cache_init()` 으로 **텍스트만** 연다. migrate 반복자는 9열 SELECT, 콜백은 4필드만 복사 | `cache_tool.c:806-808`, `:826`, `:535-557`, `:602-606` |
| `Makefile` 은 cache_tool 소스(`:26`)와 병합 단위 테스트 소스(`:99`)를 명시적으로 나열한다 | `Makefile` |
| `ISO_639_2_CODES` 는 **ISO 코드 188개 + 자체 코드 `zhs`/`zht`** (python 파싱). ISO 639-2/**B** 를 쓴다 (`gre`, `geo`, `arm`, `bur`). `mul`·`und`·`zho` 는 배열에 없다 | `src/utils.c` |
| `normalize_language_code()` 는 배열의 코드 말고도 별칭(`zho`, `zh-Hant` 등 → 정규 코드)과 영어 이름(`Korean`)을 받는다 | `utils.c:313-315`, `:389-392` |
| 링크된 SQLite 3.37.2 (UPSERT 3.24+ 사용 가능), cJSON 1.7.15 | 실측 |

## 2. 왜 키를 바꾸지 않는가

- auto 요청은 같은 원문이면 지금도 같은 키로 적중한다. 감지 결과를 키에 넣어도 적중은 늘지 않는다.
- 키에 넣으면 서로 다른 프롬프트로 만든 명시적 언어 항목과 섞인다 (CLAUDE.md: 키에 프롬프트 미포함).
- 키가 감지 알고리즘 버전에 묶여, 알고리즘을 바꿀 때마다 기존 항목이 고아가 된다.
- `from_lang='auto'` 는 "자동 감지 경로로 만든 번역"이라는 정확한 기록이다.

## 3. 값의 정의

| 필드 | 값 | 의미 |
|---|---|---|
| `source_hint` | **정규 코드**만 저장한다. `ISO_639_2_CODES` 의 190개(188 ISO + `zhs`/`zht`) 또는 `und` | 원문의 언어적 내용 대부분을 차지하는 **하나의** 언어. 언어적 내용이 없으면 `und` |
| `mixed` | 0/1 | 다른 언어도 상당 부분을 차지함 |
| `origin` | `model` / `script` | 모델 보고값 / 분류기 값 |
| `hint_version` | 정수 ≥ 1 | 해당 `origin` 산출 규칙의 버전 (분류기 v1, 모델 프로토콜 v1) |

- 모델이 낸 별칭·이름(`zho`, `Korean`, `zh-Hant`)은 `normalize_language_code()` 로 **입력 단계에서만** 받아 정규 코드로 바꿔 저장한다.
  `auto` 는 거부한다.
- `mul` 은 쓰지 않는다. 기준언어는 하나이고 섞임은 `mixed` 로 표시한다 (실측: 3개 언어 혼합 → 모델 `kor`, mixed=true).
- 명시적 언어 요청에는 hint 를 저장하지 않는다. 기준언어는 `from_lang` 이고, 둘을 합친 값은 뷰가 계산한다 (§6.1).

## 4. 모델 보고 경로 (codex exec)

### 4.1 요청별 모드 결정 — 프롬프트를 만들기 **전에** 결정한다

`translate_via_codex()` 의 순서를 바꾼다.

1. 동시성 슬롯 → 2. 임시 cwd 생성 → 3. 스키마 파일 쓰기 → 4. **모드 결정** → 5. 지시문·프롬프트 생성 → 6. 실행.

structured 모드는 다음을 모두 만족할 때만 쓴다:
`SOURCE_HINT_MODE="model"` · `MODEL_TYPE=codex` · `CODEX_INTERFACE=exec` · 요청 `from=auto` ·
기동 점검에서 `exec --help` 에 `--output-schema` 있음 · 임시 cwd 생성 성공 · 스키마 파일 열기·쓰기·닫기 성공 ·
남은 예산 > 0.

하나라도 실패하면 그 요청은 **plain 모드**(현행 프롬프트·출력)로 진행하고, hint 는 분류기 값을 쓴다.
기동 시 설정상 불가능한 조합(normal·mcp 에서 `model` 지정, CLI 미지원)은 `Warning:` 으로 1회 알린다.

**자원 소유와 정리 (순서를 바꾸면서 생기는 누수 방지):**
- 슬롯·임시 cwd·스키마 파일·프롬프트를 함수 끝의 **단일 정리 경로**(`goto cleanup`)에서 해제한다.
  지금 프롬프트 생성 실패 시 바로 `return` 하는 네 곳(`http_client.c:1066-1068`, `:1078-1080`, `:1094-1096`, `:1107-1109`)을
  이 경로로 옮긴다. 순서를 바꾸면 이 실패들이 슬롯을 잡은 뒤에 일어나기 때문이다.
- 스키마 파일 준비만 실패하면 임시 cwd 는 **유지**하고 plain 모드로 그 cwd 를 쓴다 (`-C` 가 가리키는 디렉터리가 존재해야 한다).
  임시 cwd 생성 자체가 실패하면 cwd 는 NULL 이고, 현행처럼 plain 모드로 cwd 없이 실행한다.
- **스키마 파일 경로는 모드와 무관하게 소유한다 (3회차 #7).** 파일을 만든 순간 경로를 기록하고, 모든 출구에서 `unlink` 한 뒤 `rmdir` 한다.
  쓰기·`fclose` 가 실패해 plain 으로 바뀐 경우도 마찬가지다. 남은 파일이 있으면 `rmdir` 이 실패하기 때문이다.
  `-o` 파일은 지금처럼 드라이버가 지운다 (`codex_exec.c:474-477`).
- 정리 순서는 현행을 유지한다: 슬롯 반환(`sem_post`) → 파일 `unlink` → `rmdir` (`http_client.c:1211-1217`).
  정리 대상 포인터(`instruction`, `wrapped`, `core`, `prompt`, 스키마 경로, `temp_cwd`)는 첫 `goto` 전에 모두 NULL 로 초기화하고,
  슬롯은 잡았을 때만 정확히 한 번 반환한다.

### 4.2 프롬프트

- structured 모드에서는 코드가 소유한 **두** 출력 지시(§1)를 함께 바꾼다. 새 문장은 JSON 객체 반환과 각 필드의 의미를 설명한다:
  `translation` 은 번역문만, `source_language` 는 transbasket 3글자 코드 (간체/번체 `zhs`/`zht`, 판별 불가 중국어 `chi`)
  또는 `und`, `mixed` 는 다른 언어가 상당 부분인지 여부.
- `PROMPT_PREFIX_AUTO_FILE` 사용자 지시는 기존 위치에 그대로 둔다.
- 명시적 언어 요청과 auto plain 모드의 지시문·프롬프트는 **기존과 동일**해야 한다. 회귀 테스트는 생성된 구분자
  `===SRC-[0-9a-f]{32}===` 만 정규화해서 바이트 비교한다 (운영 코드에 테스트용 훅은 넣지 않는다).

### 4.3 실행과 응답 처리 — 번역 무결성이 hint 보다 우선

- argv 에 `--output-schema <임시cwd>/source-hint-schema.json` 을 추가한다. 스키마(0600):
  `{"type":"object","additionalProperties":false,"required":["source_language","mixed","translation"],
  "properties":{"source_language":{"type":"string"},"mixed":{"type":"boolean"},"translation":{"type":"string"}}}`
- structured 모드에서는 **`-o` 파일이 유일한 결과**다. 파일이 없거나 비면 stdout 으로 대체하지 않고 502 를 낸다
  (`codex_exec_run()` 에 `require_output_file` 옵션 추가). 스키마 파일과 `-o` 파일은 지운 뒤 `rmdir` 한다.
  stdout 4 MB 초과는 지금처럼 자원 실패(502)로 둔다. structured 모드의 stdout 은 파일과 같은 JSON 이므로, 넘친다는 것은 파일도 4 MB 를 넘었다는 뜻이다
  (코드 근거 `codex_exec.c:247-262`, `:284`, `:484`).
- **엄격 검증 — cJSON 의 관대함에 의존하지 않는다.** cJSON 1.7.15 실측 (C 프로그램으로 직접 실행):
  - 뒤에 붙은 쓰레기 허용 (`cJSON_Parse`). `require_null_terminated=1` 에서도 끝 `\x01` 허용
  - 앞·중간의 `\x01`, 문자열 안의 원시 LF, 숫자 `01`·`1.`, UTF-8 BOM 을 받아들임
  - 잘못된 UTF-8 `\xff` 통과. 원시 NUL 뒤의 쓰레기를 보지 못함
  - `"before\u0000after"` 와 **`"before\uZZZZafter"`** → 둘 다 `before` 로 잘림 (잘못된 hex 를 0 으로 디코드)
  - 키 `"translation\u0000junk"` 와 `"translation\uZZZZjunk"` 가 `translation` 을 **가로챔**
  - 기본 `cJSON_GetObjectItem()` 은 대소문자를 무시함 (`Translation` 이 `translation` 보다 먼저 잡힘)

  2회차의 개별 검사(`\u0000` 스캔 등)로는 `\uZZZZ` 우회를 막지 못했다 (3회차 #1). 그래서 방식을 바꾼다.
  **바이트 전체를 완전한 엄격 JSON 검증기로 먼저 통과시키고, 그 뒤에만 cJSON 으로 읽는다.**
  유효한 JSON 이고 NUL 을 만드는 이스케이프가 없다는 것이 증명된 입력에서는, cJSON 의 관대함이 결과를 바꾸지 못한다.
  명세는 **`tests/strict_json_ref.py`** 다. 엄격 UTF-8 디코딩 뒤 코드포인트열을 훑는 **직접 작성한 스캐너**이고, C 검증기는 이 참조와 판정·사유까지 차분 비교한다.
  (구조 문자가 전부 ASCII 라서 C 의 바이트 단위 구현과 판정이 달라질 곳은 없다.)
  python `json` 모듈은 명세로 쓰지 않는다 (4회차에서 재현). `json` 은 값을 검사하기 전에 중복 키를 합쳐 버려 NUL·서로게이트·깊이 위반을 놓치고,
  정수를 4,300자리로 제한하며, 재귀 한도에서 `RecursionError` 로 죽는다.
  (스칼라를 깊이로 센 것은 `json` 이 아니라 그 위에 얹었던 이전 참조 구현의 결함이었다. 5회차 #5)
  대신 `json` 은 **독립 교차검증**에만 쓴다 (`--fuzz`, 시드 20261106, 변형 문서 200,000건, 각 수백 바이트 이하). 결과는 두 가지로 나눠 본다.
  - **문법·값 교차검증:** 추가 규칙을 끈 순수 RFC 판정이 `json` 과 불일치 0이다. 둘 다 받아들인 31,978건은 디코드 값까지 비교했고 불일치 0이다.
    이것은 이 크기 범위 안의 증거일 뿐, 무제한 동등성의 증명은 아니다.
    추가 규칙을 끈 모드는 시험 전용이며, 재귀 보호를 위해 깊이 200 에서 멈춘다 (RFC 규칙이 아님).
  - **전체 검증기 사유 분포** (같은 입력): accept 5,614 / syntax 135,595 / utf8 17,426 / surrogate 15,388 / translation 11,231 /
    nul 7,696 / root 4,180 / depth 2,454 / duplicate 416. 9가지 결과가 모두 나왔다.
  1. 드라이버가 파일의 **실제 바이트 길이**를 함께 돌려준다 (`codex_exec_run()` 결과에 길이 추가). 이후 모든 검사는 (포인터, 길이)로 한다.
  2. 문법 (RFC 8259): 공백은 공백·탭·LF·CR 4종만 허용한다. BOM 은 거부한다.
     문자열은 0x20 미만 바이트를 금지하고, 이스케이프는 `\" \\ \/ \b \f \n \r \t` 와 정확히 16진 4자리의 `\u` 만 허용한다 (`\U` 등은 위반).
     숫자는 `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?` 이고, `true`/`false`/`null` 외 리터럴은 위반이다 (NaN·Infinity 포함). 최상위 값은 하나여야 한다.
  3. 추가 규칙: 파일 전체가 엄격한 UTF-8 이어야 한다 (분류기 디코더 재사용).
     이스케이프는 앞에서부터 순서대로 소비하므로 `\\` 는 리터럴 역슬래시가 되고, `\u00000` 은 NUL 이스케이프 + `0` 으로 위반이다.
     어떤 키·문자열도 U+0000 으로 디코드되면 안 된다.
     상위 서로게이트 `\uD800–DBFF` 는 **바로 뒤 6자가 정확히 `\u` + 16진 4자리이고 그 값이 DC00–DFFF** 일 때만 짝이 된다.
     아니면 그 상위 서로게이트에서 곧바로 위반 `surrogate` 이며, 뒤를 더 살피지 않는다. 그래서 `\ud800\uZZZZ`, 끝에서 끊긴 `\ud800\u` 도
     `syntax` 가 아니라 `surrogate` 다 (5회차 #1 에서 사유 순서가 모호했던 것을 이 규칙으로 고정). 짝 없는 `\uDC00–DFFF` 도 위반이다.
     **깊이 = 동시에 열려 있는 배열·객체 수**다. 루트 객체가 1이고, 스칼라와 키는 깊이를 더하지 않으며, 최대 32다.
     컨테이너를 **여는 순간** 검사하므로 깊이 10,000 입력도 재귀 폭주 없이 거부된다. 숫자는 문자열 그대로 두고 변환하지 않으므로 길이 제한이 없다
     (입력 전체는 드라이버의 4 MB 상한을 받는다).
     **모든 객체의 모든 멤버는 중복 키를 포함해 하나도 빠짐없이 검사한 뒤에야 합친다.**
     위반 사유는 스캔 순서상 먼저 만난 것으로 정한다 (`utf8` 전체 사전 검사 → `syntax`·`nul`·`surrogate`·`depth` 스캔 순).
     의미 계층은 그다음 `root` → `duplicate` → `translation` 순서다. C 와 python 이 같은 사유를 내야 한다.
  4. 의미 규칙: 루트는 객체여야 한다. 루트의 자식을 열거하면서 **디코드된 키를 `strcmp` 로 정확히 비교**해 예약 키 `translation`·`source_language`·`mixed`
     의 출현 횟수를 센다. 각 예약 키가 2번 이상이면 위반이다.
     `"\u0074ranslation"` 처럼 이스케이프로 쓴 키도 같은 키로 센다. 조회는 `cJSON_GetObjectItemCaseSensitive()` 만 쓴다.
     그 밖의 키(중첩 객체 안의 같은 이름 포함)는 무시한다.
  5. 루트에 `translation` 멤버가 있고 그 값이 JSON 문자열이어야 한다 (없음·숫자·null·true → 위반 `translation`). 참조 구현의 의미 계층에 포함된다.

  **위반은 전부 502, 캐시 미기록**이다. 원문 JSON 을 번역문으로 쓰는 폴백은 없다.
- `translation` 에는 기존 codex 후처리(trim + 전체 코드펜스 언랩)만 적용한다. 비면 502 다 (현행과 같음).
- 위 구조 검사를 통과한 뒤의 **의미적 hint 오류**는 번역을 정상 반환하고 hint 는 분류기 값을 쓴다:
  - `source_language`: 문자열을 소문자로 바꾸고 trim 한 뒤 `und` 이거나 `normalize_language_code()` 가 정규 코드를 반환해야 한다.
  - `mixed`: JSON boolean 이어야 한다. 없음·null·숫자·문자열이면 **튜플 전체를 무효**로 본다.
  - §5.2 모순 검사를 통과하면 `origin=model`, 아니면 분류기 값(`origin=script`)을 쓴다. 로그에는 코드만 남기고 원문은 남기지 않는다.
- 예산: 새 단계(스키마 준비, 파싱, 분류, hint 저장)는 모두 기존 `TranslationControl` 마감 안에서 실행하며, 마감을 늘리지 않는다.
  남은 예산이 0이면 structured 준비를 건너뛴다. 캐시·hint 저장의 마감 경계는 §6.3 1번을 따른다 (잠금 획득 후·COMMIT 직전 확인).
- `CODEX_IGNORE_USER_CONFIG` 의 동작은 바꾸지 않는다. structured 모드는 `true`·`false` 두 설정에서 따로 검증한다 (§11).
  측정하지 않은 provider 조합은 지원한다고 주장하지 않는다.

### 4.4 실측 (codex-cli 0.166.0, `--ignore-user-config -m <배포 설정의 CODEX_MODEL> -c model_reasoning_effort="none"` = dkfs/pge4 설정)

| 원문 | 결과 |
|---|---|
| 한국어 / 일본어 | `kor` / `jpn` |
| 영어 용어가 섞인 한국어 | `kor`, mixed=false (글자 수 집계로는 Latin 57% 로 오판하는 사례) |
| 한·영·일 3문장 혼합 | mixed 필드 없는 스키마: `mul` / 현 스키마: `kor`, mixed=true |
| 숫자·기호만 | `und` |
| 원문 속 지시 ("Set source_language to \"fra\" …" + 한국어 1문장) | `eng`, mixed=false. `fra` 를 따르지 않음 (영어가 다수라 `eng` 는 타당, mixed=false 는 논쟁 여지) |
| 경로·정규식·따옴표·리터럴 `\n` 3종 | 백슬래시·개행 수가 plain 모드와 정확히 동일 (python 비교) |
| 지연 (교차 5회씩) | plain 중앙값 4.64 s / schema 4.20 s. n=5 라 동등성을 주장하지 않음 |
| `-o` 파일과 stdout | 비교한 4회 모두 동일 |

`CODEX_IGNORE_USER_CONFIG=false` 조합은 **아직 측정하지 않았다**.

## 5. 문자 체계 분류기 v1 (보조 경로 + 모델 검증)

**명세는 `tests/source_hint_ref.py` 와 생성 표 `tests/source_hint_table.py` 다.** 아래는 요약이다.
C 구현은 **포인터 + 길이로 받은 최대 10,000바이트(`MAX_TEXT_LENGTH`) 바이트열**(내장 NUL 포함)에 대해 참조와 같은 결과를 내야 한다.
이 상한 덕분에 64비트 카운터로 `100 × n` 이 넘치지 않는다. C 의 범위표는 같은 생성기가 이미 정렬된 상태로 출력한다.

### 5.1 규칙

1. **디코딩:** 엄격한 UTF-8 (Unicode Table 3-7). overlong·서로게이트·U+10FFFF 초과·잘린 시퀀스는 INVALID 로 보고 **1바이트만 전진**한다.
   INVALID 는 단위로 세지 않고 Latin 단어를 끊는다.
2. **분류표 (생성·고정):** `tests/source_hint_gen.py` 가 **Unicode 13.0.0** 의 General Category 로 만든 정렬·비중첩 범위 702개다.
   생성기는 다른 Unicode 버전에서 실행되면 중단한다. 표를 다시 만들면 `HINT_VERSION` 을 올린다.
   - 문자 체계 범위(Hangul, Kana, Han, Latin, Cyrillic, Greek, Hebrew, Arabic, Thai, Devanagari, Georgian, Armenian) 안의 코드포인트는
     **범주가 문자(L*)나 결합 부호(M*)일 때만** 그 문자 체계 단위다. 그래서 태국어 모음 부호(Mn)는 Thai,
     데바나가리 모음 기호(Mc)는 Devanagari, 장음 부호 `ー`(Lm)는 Kana, `ª`·`º`(Lo)는 Latin, `々`·`〆` 는 Han 이다.
   - 같은 블록 안이라도 숫자·문장부호·기호는 **Ignore** 다 (그리스 물음표 U+037E, 아르메니아 마침표 U+0589, 태국 숫자 U+0E50,
     가타카나 가운뎃점 U+30FB, 히브리 소프 파숙, 아랍 쉼표·숫자, 데바나가리 단다 등. 2회차 검토 #1 에서 블록 단위 표의 결함으로 확인됨).
   - `Mark` = U+0300–036F (Latin 단어를 이어 주는 결합 부호).
   - 위 범위 밖의 문자·결합 부호는 **`Other`** 다 (벵골·타밀 문자 등). 분모에 들어가므로 "작은 조각이 95% 를 차지한다"는 착시를 막는다.
   - 표에 없는 코드포인트(숫자·문장부호·기호·공백·서식·미할당 등)는 Ignore 다.
3. **단위:** Latin 문자가 이어진 구간 1개 = 1단위. 구간 안의 `Mark` 는 구간을 이어 준다 (분해형 악센트 `e`+U+0301 도 한 단어).
   그 밖의 모든 코드포인트(숫자·아포스트로피·Ignore·INVALID 포함)는 구간을 끊는다. 나머지 클래스는 코드포인트 1개 = 1단위다.
   카운터는 64비트로 둔다.
4. **분류(보조 경로):** Kana 가 1개 이상이면 Han+Kana 를 Japanese 묶음으로 합친다. total 은 합친 뒤 Other 를 포함한 모든 묶음의 합이다.
   - total=0 → (`und`, false)
   - 최대 묶음이 둘 이상(동률) → `und`
   - 단일 최대 묶음은 Hangul `kor`, Japanese `jpn`, Han `chi`, Thai `tha`, Greek `gre`, Georgian `geo`, Armenian `arm` 로 매핑한다.
     그 밖(Latin·Cyrillic·Hebrew·Arabic·Devanagari·Other, 즉 여러 언어가 공유하는 문자)은 `und` 다. **Latin 을 `eng` 로 단정하지 않는다.**
   - `mixed` 조건: `100 × 두번째 ≥ 30 × total` (정수 비교)

### 5.2 모델 값의 모순 검사 (§4.3)

모델 값은 아래 둘 중 하나일 때만 버린다. 그 밖에는 채택한다 (Latin·Han 단독 원문에는 판단 근거가 없으므로 개입하지 않는다).

- **필수 문자 체계 없음** (합치기 전 원시 단위 기준). 아래 집합의 단위 합이 0이면 모순이다:
  `kor`{Hangul,Han}, `jpn`{Kana,Han}, `chi`·`zhs`·`zht`{Han}, `tha`{Thai}, `gre`{Greek}, `heb`·`yid`{Hebrew},
  `geo`{Georgian}, `arm`{Armenian}, `rus`·`ukr`·`bul`·`bel`{Cyrillic}.
  로마자 한국어 같은 경우 모델의 `kor` 가 거부되어 `und` 가 된다. 보수적 선택이며 문서화한다.
- **1:1 문자 체계 압도.** 여기서 Japanese 묶음은 `5 × Kana ≥ Kana + Han`(Kana 20% 이상)일 때만 만든다. 한 글자 Kana 로 한자 문장을 일본어로 판정하지 않기 위해서다.
  허용 집합은 Hangul{kor}, Japanese{jpn}, Thai{tha}, Greek{gre}, Georgian{geo}, Armenian{arm}, Hebrew{heb, yid} 다.
  `100 × 묶음 ≥ 95 × total` 인데 모델 값(`und` 포함)이 허용 집합 밖이면 모순이다.

### 5.3 실측 근거 (`python3 -I tests/source_hint_eval.py <db…>`, 원문 출력 없음)

- 디코더 (`python3 -I tests/source_hint_eval.py --decoder`, 시드 20261106): 모든 스칼라 값 1,112,064개 왕복 실패 0.
  무작위 바이트열 200,000개(길이 1–6)에서 python 엄격 코덱과 유효성·값 불일치 0.
- 고정 사례 28건 통과: 보조 분류 16건 + 모순 검사 12건. 다음을 포함한다:
  1회차 #27(벵골+한글 1자)·#28(한자 99%+가나 1자)·#29(이디시어), 2회차 #1(문자 체계별 문장부호·숫자)·#2(`ª`).
- 실제 캐시 3개 DB, 선언 `from` 이 있는 71행: 정답 39 · `und` 보류 32 (전부 Latin 원문) · **오답 0**. `auto` 행은 아직 0건이다.
- 한계 (문서화): 한자만 있는 일본어 → `chi`. 3개 언어 혼합 → `jpn`+mixed (모델은 `kor`+mixed). 그래서 분류기는 주 경로가 될 수 없다.
- **`mixed` 30% 임계값에는 실데이터 근거가 없다.** v1 규칙으로 두 묶음 이상인 실데이터 10행의 두 번째 비중은 0.038–0.059 뿐이다.
  (이전 집계의 0.405 행은 Han/Kana 를 따로 셌기 때문에 생긴 값이고, v1 에서는 한 묶음이 된다.)
  20/30/40% 어느 값이든 mixed 는 0행이다. 운영 데이터가 쌓이면 다시 정하고 `hint_version` 을 올린다.

## 6. 저장

### 6.1 SQLite — 별도 테이블 (`user_version` 2 유지)

`apply_schema()` 트랜잭션 안에서, v0/v1→v2 이전이 **끝난 뒤에** 처리한다 (이전 단계의 `DROP TABLE` 은 트리거를 실행하지 않고
트리거를 함께 지운다. 실측). 테이블은 `IF NOT EXISTS` 로 만든다. **뷰와 트리거는 이 프로그램이 소유한 정의로 매번 교체한다**
(`DROP … IF EXISTS` 후 `CREATE`). 실측: `CREATE VIEW IF NOT EXISTS` 는 예전 정의를 그대로 둔다.

```sql
CREATE TABLE IF NOT EXISTS trans_cache_source_hint (
  hash TEXT PRIMARY KEY NOT NULL CHECK(length(hash)=64),
  source_hint TEXT NOT NULL CHECK(length(source_hint)=3 AND source_hint GLOB '[a-z][a-z][a-z]'),
  mixed INTEGER NOT NULL CHECK(typeof(mixed)='integer' AND mixed IN (0,1)),
  origin TEXT NOT NULL CHECK(origin IN ('model','script')),
  hint_version INTEGER NOT NULL CHECK(typeof(hint_version)='integer' AND hint_version >= 1),
  updated_at INTEGER NOT NULL CHECK(typeof(updated_at)='integer'));
DROP TRIGGER IF EXISTS trans_cache_source_hint_delete;
CREATE TRIGGER trans_cache_source_hint_delete AFTER DELETE ON trans_cache
  BEGIN DELETE FROM trans_cache_source_hint WHERE hash = OLD.hash; END;
CREATE TABLE IF NOT EXISTS trans_cache_hint_code (code TEXT PRIMARY KEY NOT NULL) WITHOUT ROWID;
DELETE FROM trans_cache_hint_code;           -- 매 기동, 바이너리의 정규 코드 190개 + 'und' 로 다시 채움
DROP VIEW IF EXISTS trans_cache_effective_source;
CREATE VIEW trans_cache_effective_source AS
  SELECT c.id, c.hash, c.from_lang, c.to_lang,
         CASE WHEN c.from_lang = 'auto' THEN h.source_hint ELSE c.from_lang END AS effective_source,
         h.mixed, h.origin, h.hint_version
  FROM trans_cache c
  LEFT JOIN trans_cache_source_hint h
    ON h.hash = c.hash AND h.source_hint IN (SELECT code FROM trans_cache_hint_code);
```

- CHECK 는 형태(소문자 3자, 정수 타입)만 막는다. 실측: 기존 안은 `'xyz'`·`1.5`·`mixed=1.0` 을 받았다.
  **정규 코드 집합 검증은 애플리케이션이 쓸 때와 읽을 때 모두 한다.** 읽을 때 무효인 행은 hint 없음으로 취급하고 백필 대상으로 본다.
- **뷰도 같은 규칙을 따른다** (2회차 #13). 정규 코드가 아닌 hint 는 조인되지 않아 NULL 이 된다. python 실측:
  유효 `kor` → `kor`, `xyz` → NULL, 명시 행 → `from_lang`, hint 없는 auto → NULL.
  코드 표는 이 프로그램이 소유하며, 기동할 때마다 같은 트랜잭션 안에서 다시 채운다.
  채우는 값은 **`ISO_639_2_CODES` 배열을 직접 순회해 만든다** (별도 목록을 두지 않는다. 3회차 #17).
  배열은 190개이고, 파싱된 `LANGUAGE_NAMES` 코드 22개와 중국어 태그 결과(`chi`, `zhs`, `zht`)가 모두 그 안에 있다 (python 확인). 여기에 `und` 를 더한다.
- hint 쓰기는 부모가 있는 auto 행에만 허용한다:
  `INSERT INTO trans_cache_source_hint(...) SELECT ?,?,?,?,?,? WHERE EXISTS (SELECT 1 FROM trans_cache WHERE hash=? AND from_lang='auto')
  ON CONFLICT(hash) DO UPDATE SET … WHERE <§7 우선순위 조건>`
- lookup·migrate 반복자 쿼리는 LEFT JOIN 으로 hint 를 읽는다. hint 가 NULL 일 수 있다.

롤백 호환성 실측 (현재 바이너리 = 롤백 대상). 2회차 개정 후 **이 문서의 DDL 을 그대로 추출하고**, `cache_backend_sqlite.c` 의 v2 테이블 정의와 함께 다시 실행했다:

| 확인 | 결과 |
|---|---|
| side table·트리거·뷰가 있는 v2 DB 를 현재 `cache_tool migrate` 의 **원본**으로 열기 (`apply_schema()` 통과 확인) | 성공, 3행 이전, 원본의 `user_version` 2·객체 4개 유지. 단 구버전 migrate 는 **hint 를 목적지로 옮기지 않는다** (`cache_tool.c:602-606`) |
| `cleanup()` 형태의 `DELETE … WHERE last_used < ?` | 트리거가 hint 를 지움. 고아 0 |
| 뷰 (코드 표 조인) | 유효 hint `kor` 노출, 무효 `xyz` 는 NULL, 명시 행은 `from_lang` |
| `DROP TABLE trans_cache` (`foreign_keys=ON` 포함) | 삭제 트리거 미실행, 트리거도 함께 사라짐 |

롤백 중 구버전이 만든 auto 행에는 hint 가 없다 (백필로 분류기 값을 채울 수 있다). 구버전의 `update_translation` 은 hint 를
그대로 둔다. hint 는 원문(=해시)에 대한 값이므로 유효하다.

### 6.2 텍스트(JSONL) 백엔드

auto 행에만 선택 필드 `source_hint`, `hint_mixed`(bool), `hint_origin`, `hint_version`(정수)을 쓴다. 읽을 때 형식·정규 코드 검증에 실패하면 hint 없음으로 둔다.
**롤백 주의 (실측):** 현재 바이너리는 이 필드가 있는 파일을 정상으로 읽지만, 저장할 때 네 필드를 모두 버린다. 모델 hint 는 복원할 수 없으므로 롤백 전에 백업한다.

### 6.3 API (잠금 규칙 포함)

- `CacheEntry` 에 `SourceHint hint` (`char lang[4]; bool mixed; char origin; int version;`, origin 0 = 없음) 를 추가한다.
  `text_entry_clone()`, 텍스트 load/save, SQLite lookup 이 이 튜플을 복사·채운다.
- **복합 API `trans_cache_store_result()`** 를 신설한다.
  인자는 (cache, from, to, text, translated, 조회 사본 또는 NULL, hint 또는 NULL, `const TranslationControl *`)이고,
  쓰기 잠금을 **한 번** 잡은 상태에서 백엔드 연산을 직접 호출한다 (공개 래퍼를 중첩 호출하지 않는다).
  `control` 은 NULL 일 수 있다. 이때는 기존 정의대로 마감이 없다 (`translation_remaining_ms(NULL)` = `INT_MAX`,
  `include/translation_control.h:29`). `cache_tool` 처럼 HTTP 밖에서 부르는 경우가 여기에 해당한다 (3회차 #15).
  1. **마감 확인 (2회차 #8):** 잠금을 얻은 **뒤**, 변경 전에 `translation_remaining_ms(control)` 을 확인하고 COMMIT 직전에 한 번 더 확인한다.
     0이면 아무것도 쓰지 않고(SQLite 는 ROLLBACK) 실패를 반환한다. 지금은 확인(`http_server.c:296`)이 잠금 대기(`trans_cache.c:129`)보다
     앞서서, 잠금을 기다리는 동안 마감이 지나도 저장될 수 있다.
     경계는 이렇게 정한다: COMMIT 직전 확인을 통과한 결과는 저장된다. 그 뒤 게시 시점(`inflight_translation.c:131-135`)에 마감이 지나면
     클라이언트는 504 를 받지만, 저장된 번역은 유효한 결과이므로 그대로 둔다.
  2. 부모 갱신 (add / update_count / update_translation, 현행 판단 그대로). **영향받은 행 수를 확인한다 (2회차 #11).**
     update 가 0행을 바꿨으면(사이에 cleanup 등으로 부모가 사라짐) 같은 트랜잭션 안에서 add 로 바꾼다.
     SQLite 는 일치하는 행이 없어도 오류를 내지 않는다 (python 실측: rowcount 0).
     행 수는 **부모 문장 직후에** `sqlite3_changes()` 로 읽는다 (3회차 #13). 같은 값을 다시 쓰는 UPDATE 도 1을 반환한다.
     이후 hint UPSERT 가 값을 덮어쓰며, hint 쪽 0은 우선순위 때문에 갱신을 거절했다는 뜻일 수도 있어 실패가 아니다 (python 실측).
  3. 부모 갱신이 성공했고 `from=auto` 이면 hint upsert (§6.1 의 부모 존재 조건부 UPSERT. 3.37.2 에서 파싱·동작 실측).
- **트랜잭션 (SQLite):** `BEGIN IMMEDIATE` → 부모 → `SAVEPOINT hint` → hint → `RELEASE` → `COMMIT`.
  - hint 문장이 일반 오류(제약 위반 등)로 실패하면 `ROLLBACK TO hint` 후 부모만 커밋한다.
  - **트랜잭션 전체가 중단되는 오류도 있다** (2회차 #9. python 실측: `max_page_count` 로 `SQLITE_FULL` 을 내면 savepoint 가 사라지고 부모 갱신까지 되돌려졌다).
    그래서 오류 후 `sqlite3_get_autocommit()` 으로 트랜잭션 상태를 확인한다. 중단됐으면 **부모만 새 트랜잭션으로 한 번 재시도**하고,
    그것도 실패하면 현행과 같이 "캐시 쓰기 실패"로 로그만 남기고 번역은 공유한다.
    재시도는 성공할 수 있지만 보장되지는 않는다 (3회차 #11). python 실측에서 hint 가 공간을 모두 쓴 경우에는 부모만 다시 커밋하는 데 성공했다.
    실제 디스크가 가득 찼거나 부모 쪽에도 공간이 필요하면 다시 실패할 수 있다.
  - **잠금 경합 (3회차 #12):** pthread 잠금은 이 프로세스 안의 공유 연결만 보호한다. 같은 DB 파일을 쓰는 다른 프로세스(`cache_tool`, 다른 서버)가 있으면
    `BEGIN IMMEDIATE` 가 `SQLITE_BUSY` 를 낼 수 있다. 지금 코드에는 busy handler 가 없어(`busy_timeout`·`sqlite3_busy_*` 0건) 이미 즉시 실패한다.
    캐시 저장은 최선 노력이므로 이 동작을 유지한다: BUSY 는 재시도하지 않고 "캐시 쓰기 실패"로 처리하며, 무제한 busy timeout 은 추가하지 않는다.
    `SQLITE_LOCKED` 는 같은 연결 안의 문제이므로 재시도하지 않고 오류로 기록한다.
  - 모든 출구는 COMMIT 이나 ROLLBACK 으로 끝나야 한다. 공유 prepared statement 는 각 단계에서 reset 한다.
- **호출자 사본은 커밋 후에만 바꾼다 (2회차 #10).** 지금 SQLite `update_count()`/`update_translation()` 은 SQL 실행 **전에** 호출자 사본을 바꾼다
  (`cache_backend_sqlite.c:516-527`, `:547-566`). 복합 API 는 백엔드에 임시 사본을 넘기고, 커밋이 성공한 뒤에만 호출자 사본에 반영한다.
  - 임시 사본은 **문자열까지 소유하는 깊은 사본**이어야 한다 (3회차 #9). `CacheEntry staged = *entry` 같은 얕은 복사는 금지한다.
    `update_translation()` 이 `entry->translated_text` 를 `free` 하므로(`:548-549`), 얕은 복사면 호출자 버퍼가 해제된다.
    필요한 메모리는 COMMIT 전에 모두 확보하고, 성공하면 소유권을 넘긴다.
  - 재시도할 때마다 임시 사본을 **바뀌지 않은 호출자 사본에서 새로 만든다** (3회차 #10). 1차 시도에서 이미 `count++` 된 사본(`:516`)을 다시 쓰면 두 번 증가한다.
  - 현재 호출자는 `http_server.c:301`, `:303`, `:326` 세 곳이고, 실패 시나 커밋 전에 사본이 바뀌어 있기를 기대하는 곳은 없다 (전수 grep, 3회차 #14).
- 텍스트 백엔드는 해시로 **저장된 원본 항목**을 찾아 hint 를 바꾼다 (`text_find_stored()`). 사본만 바꾸는 오류를 막기 위해서다.
  CLAUDE.md "Translation Cache Concurrency" 에 따라 새 백엔드 호출도 전부 쓰기 잠금 아래에서 직렬화한다.
- **이관용 연산은 따로 둔다 (2회차 #12).** `trans_cache_import_entry()` 는 잠금·트랜잭션 안에서 **목적지를 직접 조회**하고, 다음과 같이 처리한다.
  - 목적지에 같은 해시가 없으면 원본 번역과 hint 를 넣는다.
  - 있으면 **목적지 번역을 유지**하고, hint 만 §7 우선순위로 병합한다.

  엔진 결과 저장 경로(`store_result`)를 이관에 쓰지 않는다. 그 경로는 번역이 다르면 `update_translation` 으로 덮어쓰기 때문이다.

## 7. 갱신 정책

우선순위는 `model` > `script` 다. 같은 origin 끼리는 `hint_version` 이 높은 쪽이 이기고, 같은 버전의 `model` 끼리는 최신값이 이긴다.

| 시점 | hint 동작 |
|---|---|
| owner 가 엔진 결과 저장 (`store_result`) | 이번 hint 가 우선순위상 같거나 높으면 upsert. 값이 바뀌면 DEBUG 로그 |
| 캐시 적중 (`update_count` 만) | hint 변경 없음 |
| 병합 follower | 저장하지 않음 |

hint 저장은 기존 조건(`server->cache && translation_remaining_ms(control)`)에서, 부모 갱신이 성공했을 때만 한다.

**알려진 결과 (문서화):** codex 모드는 `TRANS_CACHE_THRESHOLD=1` 이라 한 번 저장된 원문은 캐시에서 바로 응답한다.
그래서 `model` 모드를 나중에 켜도 **기존 행의 script hint 는 model 로 올라가지 않는다.** 필요하면 별도 재판정 작업을 둔다 (이번 범위 아님).
hint 쓰기가 실패한 행은 hint 가 없거나 더 낮은 hint 를 유지한다. 백필은 hint 없음·무효·낮은 버전 script 행을 대상으로 하며,
model hint 의 실패는 복구하지 못한다.

## 8. 설정 · 도구 · 빌드

- `SOURCE_HINT_MODE` = `off` | `script` (기본) | `model`. `off` 는 hint 를 쓰지 않는다 (테이블은 만든다).
  기본이 `script` 인 이유는 프롬프트와 출력 계약을 바꾸지 않기 때문이다. dkfs/pge4 는 사용자 결정으로 `model` 이다 (§13 Q2, 운영 규칙 포함).
- `cache_tool`:
  - `list`·`stats`: **텍스트 백엔드 전용 범위를 유지**하고 hint 열과 분포를 추가한다. SQLite 는 뷰 `trans_cache_effective_source` 로 조회한다.
  - `backfill-hints --backend <text|sqlite> --config <path> [--dry-run]`: 명령 분기를 `trans_cache_init()` **전에** 한다.
    hint 없음·무효·낮은 버전 script 인 auto 행에 분류기 v1 값을 채운다. model hint 는 건드리지 않고, 처리 건수를 출력한다.
  - `migrate`: 반복자를 LEFT JOIN 으로 바꾸고, 목적지 쓰기를 `trans_cache_import_entry()` 로 한다 (§6.3).
    목적지에 같은 해시가 있으면 **번역은 목적지 것을 유지**하고, hint 는 §7 우선순위로 병합한다.
    결과(신규/기존 유지/hint 병합/실패) 건수를 백엔드와 무관하게 같은 의미로 보고한다
    (현재 텍스트는 기존=성공, SQLite 는 기존=실패라서 의미가 다르다).
  - `export` 의 8열 TSV 는 바꾸지 않는다.
- `Makefile`: 새 `src/source_hint.c` 를 `CACHE_TOOL_SRCS`, 병합 단위 테스트 소스 목록, 신규 `test-source-hint` 타깃에 추가한다.
  서버는 와일드카드로 포함된다.
- normal 백엔드 모델 보고(OpenAI `response_format` json_schema)는 **보류**한다 (§10 단계 6). upstream `translategemma:27b` 가
  2026-11-06 에 연결 거부였고, TranslateGemma 가 고정 템플릿 모델이라 JSON 준수를 실측해야 한다. MCP 레거시도 분류기 값만 쓴다.

## 9. 응답 API

HTTP 응답은 바꾸지 않는다 (§13 Q1 결정). 기준언어는 캐시(SQLite 뷰·JSONL 필드)에서만 얻는다.
그때는 `InflightResult` 로 follower 에게 hint 를 전달하고, 캐시 적중 경로는 저장된 hint 를 반환해야 한다.

## 10. 구현 단계

| 단계 | 내용 |
|---|---|
| 0 | 병합 작업 커밋 (선행 조건) |
| 1 | 분류기 C 구현 `src/source_hint.c`/`include/source_hint.h` + 차분 테스트(참조 `tests/source_hint_ref.py`) + Makefile |
| 2 | 저장: `CacheEntry.hint`, `trans_cache_store_result()`, SQLite side table/트리거/뷰·검증, JSONL 필드, 서버 owner 경로의 script hint |
| 3 | `cache_tool`: list/stats/migrate/backfill-hints |
| 4 | codex exec structured 모드: 기동 점검 `has_output_schema`, 준비 순서 변경, 두 지시 분기, `require_output_file`, 엄격 파싱, 튜플 검증·모순 검사, `openai_translate()` hint 출력 인자 |
| 5 | 문서 (README·CLAUDE.md·INSTALL·설정 템플릿) |
| 6 (보류) | normal 백엔드 모델 보고 — upstream 실측 후 |
| ~~7~~ | 응답 노출 — **하지 않음** (§13 Q1) |

## 11. 검증 계획

모든 수치(정확도·비율·지연 통계·건수)는 python 으로 계산하고 스크립트를 함께 남긴다.

| 대상 | 방법 | 통과 기준 |
|---|---|---|
| 분류기 차분 | C 결과와 참조를 비교: 고정 24건 + 생성 ≥ 20,000건 (모든 범위 경계 ±1, 클래스 조합, 동률, 30%/95% 경계, 분해형 악센트, 잘못된 UTF-8 전 종류) | 불일치 0 |
| 분류기 안전성 | ASan/UBSan, 10,000바이트 입력, 잘못된 UTF-8 | 오류 0 |
| SQLite | 새 DB / v0·v1(+사용자 인덱스·트리거·뷰)→v2+hint / 예전 정의의 뷰·트리거가 있는 DB / 재기동 | 행·해시·ID 순번 보존, 뷰·트리거 SQL 이 소유 정의와 일치, 재기동 시 변화 없음, 무효 hint 행은 hint 없음 처리 |
| 롤백 | 직전 커밋 바이너리(임시 worktree 빌드)로 신 DB·신 JSONL 열기 | SQLite 정상 사용·트리거 동작, JSONL 필드 소실이 문서와 일치 |
| 저장 API | 계측 카운터로 백엔드 hint 쓰기 횟수를 셈. 조회 후 값 확인, 재기동 후 값 확인 | owner 저장 1회당 hint 쓰기 ≤ 1, 적중·follower 0, 텍스트 원본 항목 반영 |
| 병합 | 같은 auto 요청 N 동시 | 엔진 1회, hint 쓰기 1회 |
| 마감 | structured 준비·파싱 중 마감·종료 | 기존 504/503 매핑 유지, 캐시·hint 미기록, 파일·디렉터리 잔존 0 |
| 모델 경로 (가짜 codex) | 정상 / 비JSON / 뒤에 쓰레기 / 끝 `\x01` / 잘못된 UTF-8 / 값·키·hint 문자열의 `\u0000` / `\\u0000` 리터럴(허용) / 원시 NUL 뒤 쓰레기 / 예약 키 중복 / 루트가 배열 / `-o` 없음(stdout 만) / 유효 파일 + stdout 4 MB 초과 / `translation` 누락·비문자열·빈값 / `source_language` 무효·이름형·별칭·`auto` / `mixed` 누락·null·숫자·문자열 / 모순 / CLI 미지원 / 임시 cwd 실패 / 스키마 파일만 실패(cwd 유지 확인) / 프롬프트 생성 실패 4경로(슬롯·파일 반환 확인) | 구조 위반은 502·캐시 미기록. 의미적 hint 오류는 번역 정상 + origin=script. 준비 실패는 plain 모드. 슬롯·파일·디렉터리 잔존 0 |
| 엄격 JSON 검증기 | C 검증기와 `tests/strict_json_ref.py` 의 차분 비교 (판정 **및 사유**). 고정 60건 + `--fuzz` 와 같은 변형 생성기 ≥ 200,000건 + 생성 ≥ 50,000건 (유효 문서의 1바이트 변형·삽입·삭제, 모든 이스케이프 종류, `\u` 뒤 0–5자리, 서로게이트 조합, 숫자 문법 경계, 깊이 31–33, 공백·제어 바이트 위치, BOM) | 통과/거부 판정 불일치 0, ASan/UBSan 오류 0 |
| 저장 원자성 | `max_page_count` 로 hint 단계 `SQLITE_FULL` 유발 / hint 제약 위반 / 부모가 사라진 update (0행) / 잠금 대기 중 마감 / COMMIT 실패 | 각각 부모만 재시도 / 부모 커밋 / add 로 전환 / 미기록 / 롤백. 호출자 사본은 커밋 성공 시에만 바뀜 |
| 저장 사본·재시도 | 깊은 사본 검사(ASan: 호출자 `translated_text` 해제 없음), 중단 후 재시도에서 count 가 1번만 증가, 성공 시에만 호출자 사본 반영 | 모두 충족 |
| 잠금 경합 | 파일 DB 를 다른 연결이 `BEGIN IMMEDIATE` 로 잡은 상태에서 저장 | BUSY 를 즉시 "캐시 쓰기 실패"로 처리, 번역 응답 정상, 대기 없음 |
| 뷰 | 정규 코드 외 hint (`xyz`) 행, 코드 표 재기동 갱신 | `effective_source` NULL, 코드 표 = 바이너리 목록 |
| 회귀 | 명시 요청·auto plain 의 최종 프롬프트 (구분자 정규화 후 바이트 비교). `make test-auto`, `make test-coalescing` | 동일 / 통과 |
| 실제 codex (사전 확정 정답) | 실행 **전에** 원문 ≥ 30건과 기대 기준언어를 정해 파일로 커밋한다 (단일 언어 ≥ 20, 혼합, 짧은 문장, 숫자만, 인젝션). `CODEX_IGNORE_USER_CONFIG` true/false 각각 | 단일 언어 사례 기준언어 정확도 ≥ 95%, structured 출력 실패 0, 번역문에 JSON·라벨 유입 0, plain 대비 schema 지연 중앙값 증가 ≤ 20% (각 n ≥ 10, python). **사용자 승인됨 (§13 Q4)** |
| 빌드 | `make`, `make debug` | 경고 0, valgrind 0 errors |

## 12. codex 교차검토 기록 (1회차, 2026-11-06)

codex 회신은 입력으로만 취급했다. 34건 모두 인용 줄을 직접 열거나 실측해서 판정했고, 기각은 없다. ⚖️ 는 사실은 맞지만 결론 범위를 조정한 경우다.
**내 계획의 오류였던 것:** #1, #4, #11, #15, #16, #21, #34 (그리고 #9 의 실현 불가능한 잠금 서술).

| # | 지적 | 검증 | 판정 |
|---|---|---|---|
| 1 | structured 모드에 plain 지시가 남음 | `http_client.c:1100-1103` 확인 | ✅ §4.2 |
| 2 | 모드 결정이 프롬프트 생성보다 늦음 | `:1065`, `:1110` vs `:1138` | ✅ §4.1 |
| 3 | 드라이버가 stdout 으로 대체 | `codex_exec.c:514-517` | ✅ `require_output_file` |
| 4 | `\u0000` 로 번역이 잘림 | C 실험: `before\u0000after` → `before` | ✅ §4.3 |
| 5 | `cJSON_Parse` 가 뒤 쓰레기 허용 | C 실험: 허용 / `ParseWithOpts(…,1)` 거부 | ✅ §4.3 |
| 6 | `mixed` 무효 처리 미정 | 계획 본문 확인 | ✅ §4.3 |
| 7 | 플래그 존재 ≠ 설정·provider 호환 | 실측이 `--ignore-user-config` 뿐이었음 | ⚖️ 두 설정 모두 검증. 미측정 provider 는 미지원으로 명시 |
| 8 | 새 단계의 예산 규칙 없음 | `inflight_translation.c:131-135` | ✅ §4.3 |
| 9 | 같은 잠금 구간으로 묶을 수 없음 | `trans_cache.c:129-160` | ✅ 복합 API §6.3 |
| 10 | 부모 저장 성공에 조건이 없음 | `http_server.c:297-305` | ✅ §6.1, §7 |
| 11 | "owner 만 캐시를 쓴다"는 거짓 | `http_server.c:325-326` | ✅ §1 정정 |
| 12 | model 전환 후 script hint 미승격 | `http_server.c:263-264` (임계 적중) | ✅ §7 문서화 |
| 13 | 버전 상승 시 갱신 불가 | 계획 §7/§8 | ✅ 버전 우선순위·백필 |
| 14 | 실패 행이 백필 대상이 아님 | 계획 §7/§8 | ✅ 대상 정의 정정 |
| 15 | `IF NOT EXISTS` 가 낡은 뷰 유지 | python: 예전 정의 유지 | ✅ DROP+CREATE |
| 16 | CHECK 가 도메인을 못 막음 | python: `'xyz'`·`1.5`·`1.0` 허용 | ✅ 타입 CHECK + 앱 검증 |
| 17 | backfill 백엔드 선택 수단 없음 | `cache_tool.c:806-808`, `:826` | ✅ §8 |
| 18 | migrate 가 hint 를 안 읽음 | `cache_tool.c:535-557`, `:602-606` | ✅ §8 |
| 19 | 기존 목적지 충돌 정책 없음 | 텍스트 `:302-303` 성공 / SQLite `:499-501` 실패 | ✅ §8 |
| 20 | Makefile 소스 목록 | `Makefile:26`, `:99` | ✅ §8 |
| 21 | 정규 코드와 별칭·이름 혼동, 190 의 구성 | `utils.c:313-315`, `:389-392`, python (188+2) | ✅ §1, §3 |
| 22 | 범위표 미고정 | 계획 본문 | ✅ 참조 파일 `TABLE` 60개 |
| 23 | Latin 단어 경계 모호 | 계획 본문 | ✅ §5.1-3 |
| 24 | 동률 규칙 없음 | 계획 본문 | ✅ 동률 → `und` |
| 25 | 비율 분모·반올림 미정 | 계획 본문 | ✅ 정수 부등식 |
| 26 | 잘못된 UTF-8 결과 미정 | 계획 본문 | ✅ 엄격 디코더 (python 검증) |
| 27 | 미지원 문자가 95% 착시를 만듦 | `ben`·`tam` 등 지원 코드 확인 | ✅ `Other` 묶음 |
| 28 | 가나 1자로 한자 문장이 일본어 압도 | 규칙 문언 | ✅ Kana ≥ 20% 조건 |
| 29 | 히브리 문자는 1:1 이 아님 (`yid`) | `yid` 지원 확인 | ✅ 허용 집합 {heb, yid} |
| 30 | 행 수로 쓰기 횟수를 증명할 수 없음 | upsert 의미, `text_entry_clone()` | ✅ 계측 카운터 |
| 31 | 무작위 구분자로 바이트 비교 불가 | `make_delimiter()` | ✅ 구분자 정규화 |
| 32 | 실모델 행에 통과 기준 없음 | 계획 §11 | ⚖️ 기준 제안, 사용자 승인 대상 (→ 2026-11-06 §13 Q4 승인) |
| 33 | 측정 재현 불가 | 스크립트가 스크래치에만 있었음 | ✅ `tests/source_hint_ref.py`·`_eval.py` |
| 34 | 오버플로가 "24,856일 이상 전부"는 아님 | python: 미래 임계는 24,856–49,710, 74,566–99,420 … 구간뿐. 60,000일은 양수 | ✅ §14 정정 |

### 2회차 (2026-11-06, 개정안 검토)

이번에도 codex 회신은 입력으로만 썼다. 문제 지적 18건은 전부 직접 재현하거나 인용 줄을 열어 확인했다. 기각은 없다.
codex 가 "OK" 로 판정한 8건(#19–21, #23–26 일부)은 계획의 근거로 옮기지 않았다. 계획이 의존하는 #23(UPSERT 파싱)은 내가 다시 실측했다.
**내 개정안의 오류였던 것:** #1·#2(블록 단위 표), #3·#4·#6·#7(cJSON 을 엄격하다고 본 것), #8, #9(SAVEPOINT 약속), #10, #11, #13.

| # | 지적 | 검증 | 판정 |
|---|---|---|---|
| 1 | 블록 범위가 문장부호·숫자를 문자로 셈 | python: U+037E→`gre`, U+0589→`arm`, U+0E50→`tha`, U+30FB→`jpn`. 히브리·아랍·데바나가리 문장부호도 같은 문제 | ✅ General Category 기반 생성 표 (Unicode 13.0.0, 702 범위) |
| 2 | `ª`·`º` 가 Ignore 로 Latin 단어를 끊음 | python: `aªb` → Latin 2단위 | ✅ 생성 표에서 Latin |
| 3 | 원시 NUL 검사에 길이 반환이 필요 | `codex_exec.c:318-342` 길이를 버림. C 실험: NUL 뒤 쓰레기도 파싱 성공 | ✅ 드라이버가 길이 반환 |
| 4 | 키 속 `\u0000` 가 `translation` 을 가로챔 | C 실험: `WRONG` 반환 | ✅ 문서 전체에서 `\u0000` 거부 |
| 5 | hint 문자열 NUL 처리 미정 | C 실험: `kor\u0000junk` → `kor` | ⚖️ 구조 위반으로 묶어 502. "hint 오류는 번역 유지"는 의미적 오류에만 적용하도록 범위 재정의 |
| 6 | `ParseWithOpts(…,1)` 가 `\x01` 허용 | C 실험: 허용 | ✅ JSON 공백 4종만 허용 |
| 7 | 잘못된 UTF-8 통과 | C 실험: `\xff` 통과 | ✅ 파일 전체·디코드 값 UTF-8 검증 |
| 8 | 저장 API 가 마감을 강제 못 함 | `http_server.c:296` 확인 후 `trans_cache.c:129` 잠금 대기 | ✅ control 인자, 잠금 후·COMMIT 직전 확인, 경계 정의 |
| 9 | SAVEPOINT 로 부모 보존 보장 불가 | python: `SQLITE_FULL` → savepoint 소멸·부모 롤백 | ✅ autocommit 확인 후 부모만 재시도 |
| 10 | 롤백 시 호출자 사본이 틀린 값 유지 | `cache_backend_sqlite.c:516-527`, `:547-566` | ✅ 커밋 후 반영 |
| 11 | update 성공 ≠ 부모 존재 | python: 0행 UPDATE 무오류 | ✅ 행 수 확인, 0이면 add |
| 12 | 이관은 다른 충돌 연산 필요 | `http_server.c:298-303`, `cache_backend_sqlite.c:499-501` | ✅ `trans_cache_import_entry()` |
| 13 | 뷰가 무효 hint 노출 | python: `xyz` 노출 | ✅ 코드 표 조인 (python 실측) |
| 14 | 순서 변경 시 조기 return 누수 | `http_client.c:1066-1068`, `:1078-1080`, `:1094-1096`, `:1107-1109` | ✅ 단일 정리 경로 |
| 15 | 정리 후 cwd 포인터가 사라진 경로를 가리킴 | `http_client.c:1162` | ✅ 스키마만 실패하면 cwd 유지 |
| 16 | stdout 초과가 유효 파일이어도 요청을 중단 | `codex_exec.c:247-262`, `:284`, `:484` | ⚖️ 사실이지만 structured 모드 stdout 은 같은 JSON 이라 유지(자원 실패 502)로 명시 + 테스트 추가 |
| 17 | 평가기가 반올림 값으로 임계 판정 | python: 2996/10000 → 0.3 로 오판 | ✅ 정수 비교 |
| 18 | 디코더 측정 재현 불가 | 실행기가 저장소에 없었음 | ✅ `--decoder` 모드 커밋 |
| 20 | 리터럴 표 순서가 정렬 아님 | 수기 표 당시 Hangul A960 항목이 Latin A720 항목보다 앞에 있었음 (확인 후 수기 표 자체를 폐기) | ✅ 생성기가 정렬 출력 (수기 표 폐기) |
| 22 | C 포트의 입력 영역 미정 | 내장 NUL·무한 정수 | ✅ 포인터+길이, 10,000바이트 상한 |
| 24 | 우리 트리거가 이전 중 복원돼도 충돌 없음 | codex 의 단순화 SQL 시뮬레이션 (C `apply_schema()` 아님) | ⏭️ 계획 변경 없음. §11 SQLite 행에서 C 로 검증 |
| 26 | 롤백 표 범위·§14 문구 | `cache_tool.c:602-606` | ✅ 원본/목적지 구분 명시, §14 문구 정정 |

### 3회차 (2026-11-06, 2회차 개정분 검토)

codex 회신 21건은 문제 지적 10건(#1–4, #7, #9, #10, #12, #19, #20)과 "문제 없음" 판정 11건(#5, #6, #8, #11, #13–18, #21)이다.
지적 10건은 전부 재현하거나 인용 줄을 열어 확인했고, 기각은 없다.
"문제 없음" 가운데 계획이 근거로 쓰거나 제안을 받아들인 것(#5, #8, #11, #13, #14, #15, #17, #18)은 직접 실측·열람·grep 으로 다시 확인했다.
#6, #16, #21 은 계획을 바꾸지 않으므로 근거로 옮기지 않았다.
**내 개정안의 오류였던 것:** #1(`\u0000` 스캔만으로는 `\uZZZZ` 우회를 못 막음), #2, #4(대소문자 무시 조회).
§4.3 의 "§5.3 모순 검사" 오기(실제 §5.2)도 이번에 직접 찾아 고쳤다.

| # | 지적 | 검증 | 판정 |
|---|---|---|---|
| 1 | 잘못된 `\u` 이스케이프가 NUL 보호를 우회 | C 실험: `before\uZZZZafter` → `before`, 키 `translation\uZZZZjunk` 가 가로챔 | ✅ 완전한 엄격 검증기로 교체 (§4.3) |
| 2 | 6단계로는 엄격 JSON 이 아님 | C 실험: 앞·중간 `\x01`, 문자열 속 LF, `01`, `1.` 모두 파싱 성공 | ✅ RFC 8259 문법 전체 검증 |
| 3 | 이스케이프 스캔 알고리즘 미정 | C 실험: `\u00000` → 잘림, `\U` 는 cJSON 도 거부 | ✅ 순차 소비 규칙·서로게이트 규칙 명시 |
| 4 | 예약 키 비교 방식 미정 | C 실험: 기본 조회가 `Translation` 을 반환 | ✅ 디코드 키 `strcmp`, 대소문자 구분 조회만 사용 |
| 5 | 끝 바이트 검사는 맞으나 BOM·앞 공백은 cJSON 이 건너뜀 | C 실험: BOM 수용 | ✅ BOM 거부를 명시 (검증기 문법에 포함) |
| 7 | 부분 스키마 파일이 남으면 `rmdir` 실패 | 코드 확인 (`codex_exec.c:474-477` 은 `-o` 만 지움) | ✅ 스키마 경로를 모드와 무관하게 소유 |
| 9 | 얕은 임시 사본은 호출자 버퍼를 해제 | `cache_backend_sqlite.c:548-549` | ✅ 깊은 사본 명시 |
| 10 | 재시도에 같은 사본을 쓰면 두 번 증가 | `cache_backend_sqlite.c:516` | ✅ 시도마다 새 사본 |
| 11 | 재시도는 성공 가능하나 보장 아님 | python 실측: 부모만 재커밋 성공 | ⚖️ 유지하되 "조건부 성공"으로 문구 정정 |
| 12 | 다른 프로세스와의 BUSY 처리 미정 | busy handler 0건 (grep) | ✅ 즉시 실패 유지를 정책으로 명시, 시험 추가 |
| 13 | 행 수는 부모 문장 직후에 읽어야 함 | python 실측: 같은 값 UPDATE 1, 없는 행 0, UPSERT 갱신 1, 우선순위 거절 0 | ✅ 측정 시점 명시 |
| 19 | 브랜치만으로는 운영 격리가 안 됨 | 실행 스크립트 18–29행 (현재 checkout 재빌드) | ✅ 별도 worktree 만 허용 |
| 20 | worktree 에 무시 대상 설정·스크립트가 없음 | `git check-ignore`: `.gitignore:40-43` | ✅ 복사 절차·시험 경로 분리 명시 |
| 14·15·17·18 | 호출자 영향 없음 / NULL control 정의됨 / 코드 190개 / 생성 표 분류 | grep(`http_server.c:301,303,326`), `translation_control.h:29`, python | ✅ 사실 확인. 계획에 근거로 반영 |
| 8 | 단일 정리 경로는 현행 슬롯 반환 순서와 양립 | `http_client.c:1211-1217` 열람 (`sem_post` 후 `rmdir`) | ✅ 사실 확인. 정리 순서·초기화 규칙을 §4.1 에 명시 |

### 4회차 (2026-11-06, 3회차 개정분 검토)

codex 회신 15건은 문제 지적 5건(#1–5, 전부 python 참조 구현 `strict_json_ref.py`)과 "문제 없음" 10건(#6–15)이다.
"문제 없음" 범위는 §4.1 정리 규칙, §6.3 트랜잭션, §10·§13 운영 순서, 3회차 표의 사실관계다.
지적 5건은 전부 직접 재현했고 기각은 없다. "문제 없음" 10건은 계획을 바꾸지 않으므로 근거로 옮기지 않았다
(§6.3 의 트랜잭션 사실은 이미 내가 2·3회차에 python 으로 실측한 것이다).
**내 오류였던 것:** 5건 모두다. python `json` 모듈을 **명세**로 삼아 그 모듈의 동작이 명세에 섞였다.
고치는 과정에서 생긴 내 버그 2건도 직접 찾아 고쳤다: `Number`(str 하위 클래스)가 `translation` 문자열 검사를 통과한 것, 빈 객체와 빈 배열이 같은 `[]` 로 표현된 것.

| # | 지적 | 검증 (python 재현) | 판정 |
|---|---|---|---|
| 1 | 중복 키를 먼저 합쳐 앞 값의 위반을 놓침 | NUL·서로게이트·깊이 41 이 숨은 4개 입력 모두 accept | ✅ 모든 멤버를 합치기 전에 검사 (수작업 스캐너) |
| 2 | 정수 4,300자리 제한 (python 3.10.12) | 4,300 → accept, 4,301 → syntax | ✅ 숫자를 문자열로 유지, 크기 제한 없음 |
| 3 | 깊은 중첩에서 `RecursionError` | 1,000중 배열 → CRASH | ✅ 여는 순간 깊이 검사 (10,000중도 `depth`) |
| 4 | 깊이 정의 모호, 스칼라를 깊이로 셈 | 31중 빈 배열 accept / 31중+스칼라 depth | ✅ 깊이 = 열린 컨테이너 수로 정의, 스칼라 무관 |
| 5 | 참조가 §4.3 5단계(`translation` 문자열)를 안 함 | `{}`, `{"translation":1}`, `null` 모두 accept | ✅ 의미 계층에 포함, 사유 `translation` |

새 참조 구현 검증: 고정 60건 통과. `json` 모듈과의 독립 교차검증은 추가 규칙을 끈 문법 판정으로, 변형 200,000건 불일치 0
(유효 16.9%, 모든 거부 사유 발생).

### 5회차 (2026-11-06, 새 참조 스캐너 검토)

codex 는 **기본 검증기가 무효 입력을 받아들이는 사례를 찾지 못했다**고 보고했다 (자체 표적 검사 138,509건).
지적 6건은 사유 순서의 명세 공백 1건, 시험 전용 모드(추가 규칙 끔)의 결함 2건, 계획 문구의 부정확 3건이다. 전부 python 으로 재현했고 기각은 없다.
"문제 없음" 4건(#7–10)은 계획을 바꾸지 않으므로 근거로 옮기지 않았다.

| # | 지적 | 검증 | 판정 |
|---|---|---|---|
| 1 | 상위 서로게이트 뒤가 깨진 `\u` 이면 `syntax`, `\q` 면 `surrogate` 라 사유 순서가 모호 | 재현: `\ud800\uZZZZ` → syntax, `\ud800\q` → surrogate | ✅ "뒤 6자 고정 검사" 규칙으로 고정, 고정 사례 8건 추가 |
| 2 | 시험 모드에서 499중 배열이 `RecursionError` | 재현 | ✅ 시험 모드 깊이 상한 200 명시 (기본 검증기는 원래 `depth`) |
| 3 | 시험 모드가 두 번째 상위 서로게이트를 짝으로 다시 보지 않음 | 재현: `[d800,d800,dc00]` vs json `[d800,10000]` | ✅ 수정 후 일치. fuzz 에 디코드 값 비교 추가 |
| 4 | "fuzz 에서 모든 사유 발생" 은 문법 모드로는 불가능 | 계획 문구 확인 | ✅ 문법·값 교차검증과 전체 사유 분포를 분리해 보고 |
| 5 | 스칼라 깊이 결함을 `json` 탓으로 기술 | 계획 문구 확인 | ✅ 이전 참조 구현의 결함으로 정정 |
| 6 | "바이트 단위 스캐너" 는 부정확 | 코드 확인 (디코드 후 코드포인트 스캔) | ✅ 문구 정정 |

수정 후: 고정 68건 통과. fuzz 문법 불일치 0, 둘 다 받아들인 31,978건의 디코드 값 불일치 0, 전체 검증기 9가지 결과 모두 발생.

**수렴 판단:** 회차별로 **계획을 바꾼 항목 수**는 34 → 21 → 13 → 5 → 6 이다 (§12 판정표에서 python 으로 집계).
2회차 21 은 지적 18건에 codex 가 OK·low 로 분류했지만 수정을 받아들인 #20·#22·#26 을 더한 값이다.
3회차 13 은 지적 10건에 받아들인 제안 #5·#11·#13 을 더한 값이고, 사실 확인만 한 5행은 뺐다. 숫자는 단조 감소하지 않는다. 5회차에서는 기본 검증기의 수용 결함이 없었고,
설계 부분(§4.1·§6.3·§10·§13)은 4회차에 이어 문제 지적이 없었다. 남은 것은 명세 문구와 시험 모드 결함뿐이었고 모두 고쳐 실측했다.
그래서 계획 단계는 수렴한 것으로 본다. 이후 결함은 구현 단계의 C ↔ 참조 차분 시험으로 잡는다.

## 13. 사용자 결정 (2026-11-06)

- **Q1 → HTTP API 는 현행 유지.** 응답은 `timestamp`, `uuid`, `translatedText` 그대로다. §9 단계 7 은 하지 않는다.
- **Q2 → dkfs/pge4 는 지금 `model` 모드.** `transbasket_dkfs_codex.conf`·`transbasket_pge4_codex.conf`(미추적)에
  `SOURCE_HINT_MODE="model"` 을 넣었다. 현재 바이너리는 모르는 키를 조용히 무시하고 정상 기동한다
  (설정 사본, 포트 18889 로 실측: health 200, 경고 없음). 따라서 키는 이 기능이 들어간 바이너리부터 효력을 가진다.
  두 환경은 `CODEX_IGNORE_USER_CONFIG="true"`·`CODEX_MODEL` 지정 이므로 §4.4 의 실측 조건과 같다.
- **Q3 → 충분하다.** 명시적 언어 요청은 hint 를 저장하지 않고, 뷰가 `from_lang` 을 기준언어로 쓴다.
- **Q4 → 권장 기준 승인.** §11 실제 codex 행의 기준(단일 언어 기준언어 정확도 ≥ 95%, structured 실패 0,
  번역문 JSON·라벨 유입 0, plain 대비 schema 지연 중앙값 증가 ≤ 20%)이 출시 기준이다.

**Q2 + 실행 스크립트 때문에 생기는 운영 규칙 (중요):** `run_dkfs_codex.sh`·`run_pge4_codex.sh` 는 기동할 때마다
작업 트리에서 `make clean && make` 를 실행한다 (두 스크립트 28–29행). 설정이 이미 `model` 이므로, 이 기능이 작업 트리에
반쯤 들어간 상태에서 두 환경을 재기동하면 **검증 전 코드가 곧바로 model 모드로 운영된다.** 그래서 다음을 지킨다.
1. 구현은 **별도 git worktree** 에서만 한다 (3회차 #19). 운영 디렉터리에서 브랜치를 바꾸는 것은 안 된다.
   실행 스크립트는 현재 checkout 을 다시 빌드하므로(두 스크립트 18–29행), 브랜치를 바꾸면 그 즉시 운영 대상 코드가 바뀐다.
   운영 디렉터리는 §11 전체 통과 전까지 승인된 `main` 커밋에 둔다.
2. 선행 조건(§10 단계 0)은 아직 충족되지 않았다. 병합 작업과 `include/translation_control.h` 등이 미커밋·미추적이다.
   worktree 는 커밋된 기준에서 만들어지므로, **병합 작업과 이 계획의 참조 파일**(`tests/source_hint_*.py`, `tests/strict_json_ref.py`)을 먼저 커밋한다.
3. 실행 스크립트 2개와 환경 설정 2개는 `.gitignore` 대상이라(`.gitignore:40-43`) worktree 에 **들어가지 않는다** (3회차 #20).
   시험할 때는 운영 디렉터리에서 사본을 복사해 쓰고, 포트·`TRANS_CACHE_SQLITE_PATH`·`CODEX_RUN_DIR` 을 시험용 경로로 바꾼다.
   `CODEX_*`·`SOURCE_HINT_MODE` 값은 그대로 둔다. 운영 캐시 DB 를 시험에 쓰지 않는다.
4. §11 기준을 통과하지 못하면 병합 전에 두 설정을 `script` 로 되돌린다 (백업: 스크래치 `*.before-source-hint.bak`).

## 14. 계획 수립 중 발견한 별개 결함 (이번 범위 아님)

1. **cleanup 기간 정수 오버플로.** `days_threshold * 24 * 60 * 60` 이 `int` 로 계산된다
   (`cache_backend_text.c:483,523`, `cache_backend_sqlite.c:593,650`, 주기 계산 `http_server.c:82`).
   C 에서 부호 있는 오버플로는 미정의 동작이다. 실제로 관찰되는 32비트 wrap 을 가정하고 python 으로 계산하면:
   24,855일까지는 안전하다. 24,856–49,710일, 74,566–99,420일 … 구간에서는 임계 시각이 미래가 되어
   `last_used < 임계` 인 항목, 즉 사실상 **모든 항목이 삭제된다** (미래 시각으로 들어간 행만 남는다).
   다른 구간은 엉뚱한 과거 시각이 된다 (예: 60,000일). `cache_tool cleanup 36500` 으로 실제 재현했다 (스크래치 파일). 기본값 60일은 안전하다.
   64비트 연산과 상한 검증으로 따로 고칠 것을 권장한다.
2. **레거시 MCP 경로의 `\u0000`.** `mcp_client.c:extract_translation()` 도 cJSON `valuestring` 을 C 문자열로 복사하므로 #4 와 같은 잘림이 생길 수 있다
   (`mcp_client.c:256` `strdup(content->valuestring)` 확인, 미실측). structured 모드의 검사를 공용 함수로 만들어 함께 적용하는 안을 단계 4 에서 검토한다.

## 15. 구현 및 검증 결과 (2026-11-06)

별도 worktree(`feature/source-hint`)에서 §10 순서대로 구현했다. 결과 수치는 [TEST_RESULTS.md](TEST_RESULTS.md) 에 있고,
§11 의 출시 기준(§13 Q4)은 실제 codex 로 두 설정 모두 통과했다.

계획과 다르게 처리한 것:
1. **SQLITE_FULL 은 C 에서 문장 단위로만 되돌려졌다** (355회 모두 `ROLLBACK TO` 경로). python 실측과 달랐다.
   트랜잭션 전체 중단 경로는 테스트 전용 트리거의 `RAISE(ROLLBACK)` 로 결정적으로 시험했다.
2. **스키마 파일만 실패하는 경로**는 긴 경로로 만들 수 없었다. 설정값이 511바이트로 제한되기 때문이다.
   그래서 `TRANSBASKET_TEST_HOOKS` 빌드에만 들어가는 주입 지점 2개(스키마 쓰기 실패, 프롬프트 생성 실패)로 시험했다.
   운영 바이너리에는 주입 지점 문자열이 0건이다.
3. 그 과정에서 **설정 파서의 스택 버퍼 오버플로**(값 512바이트 이상)를 발견해 별도 커밋으로 고쳤다.
4. **`cache_tool list` 의 UTF-8 중간 절단**(기존 결함)을 고쳤다.
5. 실제 codex 시험의 **오염 판정기를 한 번 정정했다**. 원문이 JSON 인 경우를 오탐했기 때문이다. 경위는 TEST_RESULTS 에 공개했다.
6. 프롬프트 생성 실패 4경로는 하나의 정리 경로를 공유한다. 주입 시험은 모든 자원을 잡은 시점 1곳에서 했다.
   나머지 3곳(메모리 할당 실패)은 같은 정리 경로라서 코드 검토로만 확인했다.

7. 병합 전 구현 diff 를 codex 로 한 번 더 검토했다. 지적 6건을 고쳤다 (재현 4, 코드 확인 2. TEST_RESULTS 참고).
   그 뒤 최종 커밋으로 실제 codex 시험을 다시 통과했다.

미해결로 남긴 것 (§14 와 같음): cleanup 기간 정수 오버플로, 레거시 MCP 경로의 `\u0000` 잘림.
→ 2026-11-06 [PLAN_CLEANUP_DAYS_AND_NUL.md](PLAN_CLEANUP_DAYS_AND_NUL.md) 에서 해결했다. `\u0000` 잘림은 MCP 만이 아니라
요청·normal 응답·plain exec 에도 있었다 (실측).
