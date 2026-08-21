# PLAN: 중국어 간체/번체(`zh-Hans`/`zh-Hant`) 요청 지원

작성일: 2026-08-21
대상 브랜치: main (작업 시 별도 feature 브랜치 권장)
상태: **구현 완료(IMPLEMENTED)** — 2026-08-21. 검증 결과는 `TEST_RESULTS.md` 참조
채택 방식: **Option A** — 외부 API 는 BCP-47 서브태그를 그대로 받고, 내부에서는 3글자 코드로 정규화
교차검토: codex(codex-cli 0.148.0, read-only) 1회 → 전건 재검증 후 반영. 판정 내역은 §11.

---

## 1. 목표

외부 클라이언트가 `from`/`to` 에 **`zh-Hans` / `zh-Hant`(및 그 변형)** 을 보내면 간체/번체를 구분해 번역하고,
그 결과가 서로 섞이지 않도록 캐시까지 분리한다.

비목표(이번 범위 제외):
- 중국어 외 언어의 지역 변형(`en-US`, `pt-BR` 등) 일반 지원
- 광둥어(`yue`, `zh-yue`) 등 방언 코드 지원
- 캐시 스키마를 BCP-47 태그 저장으로 확장(= Option B)

---

## 2. 현황 (2026-08-21 실측)

### 2.1 현재 동작

| 요청 `to` | 결과 |
|---|---|
| `zh-Hans` / `zh-hans` / `ZH-HANT` / `zh-Hant-TW` | **422** VALIDATION_ERROR |
| `zh` / `zho` / `cmn` / `zh-CN` / `zh-TW` | **422** |
| `"Chinese (Simplified)"` (이름) | **422** |
| `chi` / `CHI` / `"Chinese"` | 200 — 결과는 **간체 고정** |

수신 경로 자체는 견고하다. 10KB 코드 문자열, 비ASCII(`中文`), 숫자/`null` 타입, 빈 문자열, 뒤 공백(`"chi "`)
모두 오버플로·크래시 없이 422 로 거부되고 서버는 정상 유지된다. `TranslationRequest` 는 `calloc` 이라
`strncpy` 널 종단도 안전하다.

### 2.2 막는 지점

| # | 계층 | 위치 | 내용 |
|---|---|---|---|
| ① | 요청 검증 | `src/utils.c:normalize_language_code()`, 호출부 `src/json_handler.c:82,102` | **정확히 3글자**이면서 하드코딩 `ISO_639_2_CODES`(188개)에 있거나, 하드코딩 `LANGUAGE_NAMES`(20개)의 이름과 일치해야 통과. `language_tables.conf` 는 검증에 쓰이지 않음 |
| ② | 요청 버퍼 | `include/json_handler.h:10-11` | `from_lang[4]` / `to_lang[4]` — `zh-Hans` 는 8바이트 필요 |
| ③ | 캐시 버퍼 | `include/trans_cache.h:16-17` | 동일 |
| ④ | 캐시 스키마 | `src/cache_backend_sqlite.c:43-44`, `cache_sqlite_schema.sql:19-20` | `CHECK(length(from_lang) = 3)` 이 **기존 DB 테이블에 이미 박혀 있음**. 실측: 7글자 INSERT → `CHECK constraint failed`. 3글자는 통과 |
| ⑤ | 관리 도구 | `src/cache_tool.c:140` | `validate_language_code()` 로 3글자만 허용 |

### 2.3 이미 준비된 지점

| 계층 | 상태 |
|---|---|
| `language_tables.conf` | `zh-Hans`, `zh-Hant` + 지역 변형 9종 등록됨(1225-1233행) |
| `language_table_resolve()` | 실측: `zh-Hans` → name `Chinese (Simplified)`, iso639_1 `zh`, iso639_2 `chi`. `zh-Hant`, `zh-Hant-HK` 도 정상. 조회는 `strcasecmp`(대소문자 무시) |
| 모델(codex) | 실측: 이름이 `Chinese (Traditional)` 이면 번체(`會議/準備/資料`), `Chinese (Simplified)` 면 간체(`会议/准备/资料`) 산출. **`{TARGET_CODE}` 값과 무관하게 `{TARGET_LANG}` 이름이 스크립트를 결정** (`zh`/`zht` 둘 다 동일 결과) |
| 응답 스키마 | 언어 코드가 응답에 없음(`timestamp`/`uuid`/`translatedText`) → 클라이언트 호환성 영향 없음 |

### 2.4 부수 발견 (같이 수정)

- `language_tables.conf:1223` `zh-CH="Chinese"` — `CH` 는 스위스. **`zh-CN` 오타**. 실측상 `zh-CN` 은 테이블 조회 실패(rc=-1).
- `language_tables.conf:1224` `zh-TW="Chinese"` — 대만은 번체이므로 `"Chinese (Traditional)"` 이 정확.

---

## 3. 설계 결정

### 3.1 Option A 채택 (사용자 승인)

**외부 입력**: `zh-Hans`, `zh-Hant`, `zh-CN`, `zh-TW` 등 BCP-47 스타일 태그를 그대로 수용
**내부 표현**: 3글자 사설 코드 `zhs`(간체) / `zht`(번체) 로 정규화

근거:
- 버퍼(`[4]`)·SQLite `CHECK(length=3)`·기존 캐시 DB 를 **전혀 건드리지 않는다** → 마이그레이션 불필요, 회귀 위험 최소
- 캐시 키가 `zhs`/`zht` 로 자연히 분리되어 간체·번체 결과가 섞이지 않는다
- `zhs`/`zht` 는 **응답 스키마에는 나타나지 않지만, 요청 값으로는 공식 지원한다**(사용자 결정).
  BCP-47 태그(`zh-Hans`/`zh-Hant`)와 3글자 코드(`zhs`/`zht`) 양쪽을 모두 문서화된 입력으로 받는다.
  3글자 코드만 다루는 클라이언트는 변환 없이 `zhs`/`zht` 를 그대로 보내면 된다.
  (초안의 "API 표면에 전혀 드러나지 않는다"는 서술은 부정확했다 — 교차검토 지적 반영)

`zhs`/`zht` 는 ISO 639-2 미할당이며 기존 배열과 충돌하지 않는다(존재하는 것은 Zhuang 의 `zha` 뿐).

### 3.2 Option B(BCP-47 그대로 저장) 기각 사유

버퍼 `[4]`→`[16]` 확장(2개 헤더 4필드), SQLite `CHECK` 완화 + **기존 DB 테이블 재생성 마이그레이션**,
검증을 언어 테이블 기반으로 교체(파싱 함수에 테이블 주입), `cache_tool` 수정이 필요하다.
간체/번체 구분이라는 현재 요구에 비해 변경 범위와 운영 위험(마이그레이션 실패 시 캐시 전량 손실)이 과도하다.
다국어 지역 변형이 실제 요구로 올라오면 그때 별도 과제로 재검토한다(§8 참조).

---

## 4. 정규화 정책 (확정안)

입력은 **대소문자 무시**, 구분자 `_` 는 `-` 로 치환한 뒤 **서브태그 단위로 분해**해 판정한다
(Java/Android 로케일 표기 `zh_Hans` 수용).

판정 규칙 (교차검토 반영, 순서 중요):

1. 전체 길이가 `LANG_TAG_MAX`(32) 이상이면 중국어 처리를 하지 않는다(기존 경로로 넘겨 422).
2. `-` 로 분해한 각 서브태그는 **비어 있지 않은 `[a-z0-9]{1,8}`** 이어야 한다. 하나라도 어긋나면 **422**.
   → `zh-Hans-`, `zh-Hans--CN`, `zh-Hans-中文`, `zh-Hans-!!!` 는 모두 거부된다.
3. 첫 서브태그가 `zh` 가 아니면 중국어 처리 대상이 아니다(기존 경로로 넘김).
4. 나머지 서브태그 중 **스크립트(`hans`/`hant`) 가 있으면 그것이 최우선**으로 결과를 정한다.
   → `zh-Hant-CN` = `zht`, `zh-TW-Hans` = `zhs` (지역보다 스크립트 우선)
5. 스크립트가 없으면 **지역 서브태그**로 판정한다: `cn`/`sg`/`my` → `zhs`, `tw`/`hk`/`mo` → `zht`.
6. 스크립트도 지역도 없으면(`zh` 단독, `zho`) → `chi`.
7. 알 수 없는 스크립트/지역만 있는 경우(`zh-xx`, `zh-latn`) → **422**.

| 입력 (소문자 정규화 후) | 결과 코드 | 프롬프트 언어명 | 비고 |
|---|---|---|---|
| `zh-hans`, `zh-hans-*` (예: `zh-Hans-HK/MO/MY/SG/TW`) | `zhs` | Chinese (Simplified) | 스크립트 우선 |
| `zh-cn`, `zh-sg`, `zh-my` (+ 유효한 하위 태그) | `zhs` | Chinese (Simplified) | 지역 판정 |
| `zh-hant`, `zh-hant-*` (예: `zh-Hant-HK/MO/MY/TW/CN`) | `zht` | Chinese (Traditional) | 스크립트 우선 |
| `zh-tw`, `zh-hk`, `zh-mo` (+ 유효한 하위 태그) | `zht` | Chinese (Traditional) | 지역 판정 |
| `zh`, `zho` | `chi` | Chinese | **기존 `chi` 와 동일 취급** — 캐시 공유, 모델 기본값(간체) |
| `zhs`, `zht` (직접 입력) | 그대로 | 각각 | **공식 지원 입력** — 3글자만 쓰는 클라이언트용 |
| `chi`, `Chinese` | `chi` | Chinese | **기존 동작 그대로 유지** |
| `"Chinese (Simplified)"` / `"Chinese (Traditional)"` (이름) | `zhs` / `zht` | 각각 | 이름 매칭 경로로 지원 |
| `cmn`, `yue`, `zh-yue`, `zh-latn`, `zh-xx` | 422 | — | 범위 외(방언/로마자/미지의 서브태그) |
| `zh-Hans-`, `zh-Hans--CN`, `zh-Hans-中文`, `zh-Hans-!!!` | 422 | — | **서브태그 문법 위반** (교차검토 반영) |

**`zh` → `chi` 로 정한 이유**: 스크립트를 지정하지 않은 요청에 서버가 임의로 "간체"를 단정하지 않는다.
`chi` 로 보내면 프롬프트 언어명이 `Chinese` 가 되어 기존과 완전히 동일하게 동작하고(모델 기본값은 간체),
**기존 `chi` 캐시 항목을 그대로 재사용**하므로 캐시 파편화와 재번역 비용이 발생하지 않는다.

**`zhs`/`zht` 로 정한 이유**: 3글자 제약을 지키면서 의미가 드러나고, ISO 639-2 와 충돌하지 않는다.

---

## 5. 상세 변경 계획

### 5.1 `src/utils.c` — 정규화 로직 (핵심)

```c
#define ZH_SIMPLIFIED  "zhs"
#define ZH_TRADITIONAL "zht"
#define LANG_TAG_MAX   32   /* 정규화 버퍼. 초과 입력은 중국어 처리 없이 통과시킴 */
#define LANG_SUBTAG_MAX 8   /* BCP-47 서브태그 최대 길이 */

/* 스크립트 서브태그 (지역보다 우선) */
static const struct { const char *sub; const char *code; } ZH_SCRIPTS[] = {
    {"hans", ZH_SIMPLIFIED}, {"hant", ZH_TRADITIONAL}, {NULL, NULL}
};

/* 지역 서브태그 (스크립트가 없을 때만 사용) */
static const struct { const char *sub; const char *code; } ZH_REGIONS[] = {
    {"cn", ZH_SIMPLIFIED},  {"sg", ZH_SIMPLIFIED},  {"my", ZH_SIMPLIFIED},
    {"tw", ZH_TRADITIONAL}, {"hk", ZH_TRADITIONAL}, {"mo", ZH_TRADITIONAL},
    {NULL, NULL}
};

/* 입력을 소문자 + '_'->'-' 로 접어 buf 에 담는다.
 * - 길이가 buf_size 이상이면 -1
 * - tolower() 인자는 반드시 (unsigned char) 로 캐스팅 (음수 char 는 UB) */
static int fold_lang_tag(const char *in, char *buf, size_t buf_size);

/* 접힌 태그를 '-' 로 분해해 판정한다. 중국어 태그가 아니면 NULL. */
static const char *normalize_chinese_tag(const char *folded);
```

`normalize_chinese_tag()` 알고리즘 (§4 판정 규칙 구현):

```
1. folded 를 '-' 로 순회하며 서브태그를 하나씩 꺼낸다.
   - 서브태그가 비어 있거나(연속 '-' 또는 끝의 '-'), 길이가 LANG_SUBTAG_MAX 초과이거나,
     [a-z0-9] 이외의 바이트를 포함하면 즉시 NULL 반환  → 최종 422
2. 첫 서브태그가 "zh" 가 아니면 NULL 반환(기존 경로로 넘김).
   단 "zho" 는 첫 서브태그가 "zho" 이고 서브태그가 하나뿐일 때만 chi 로 처리.
3. 두 번째 이후 서브태그를 모두 훑어 ZH_SCRIPTS 에 일치하는 것이 있으면 그 코드를 반환(스크립트 우선).
4. 스크립트가 없으면 같은 방식으로 ZH_REGIONS 를 훑어 일치하면 그 코드를 반환.
5. 서브태그가 "zh" 하나뿐이면 "chi" 반환.
6. 그 밖(알 수 없는 서브태그만 존재)에는 NULL 반환 → 422.
```

> **교차검토 반영**: 초안은 "접두 일치 + 다음 문자가 `'-'`" 규칙이었다. 프로토타입 실측 결과
> `zh-Hans-`, `zh-Hans--CN`, `zh-Hans-中文`, `zh-Hans-!!!` 가 모두 `zhs` 로 통과했고,
> `zh-CN-Hant` 는 지역이 먼저 매칭되어 `zhs` 가 되었다(스크립트와 모순). 위 서브태그 분해 방식은
> 두 문제를 함께 해소한다.

`normalize_language_code()` 수정:

```c
const char *normalize_language_code(const char *lang_input) {
    if (!lang_input) return NULL;

    /* (신규) 중국어 태그 우선 처리 — 3글자 분기보다 먼저 와야 "zho" 가 걸린다 */
    char folded[LANG_TAG_MAX];
    if (fold_lang_tag(lang_input, folded, sizeof(folded)) == 0) {
        const char *zh = normalize_chinese_tag(folded);
        if (zh) return zh;
    }

    /* 이하 기존 로직 (3글자 ISO 639-2, 그다음 언어명 매칭) 그대로 */
    ...
}
```

- 길이 가드: `fold_lang_tag()` 가 `LANG_TAG_MAX` 이상 입력에 `-1` 을 반환해 스택 버퍼 오버플로를 막는다.
  초과 입력은 기존 경로로 넘어가 최종적으로 422 가 된다(현재 10KB 코드 입력이 422 인 동작 유지).
- 반환 포인터는 모두 **정적 저장 기간**(문자열 리터럴 / 정적 배열)이라 기존 계약(`const char *`, 해제 불필요)을 유지한다.

`ISO_639_2_CODES` 배열에 `"zhs"`, `"zht"` 추가 (정렬 위치는 `zha` 뒤):
- `validate_language_code()` 가 두 코드를 허용 → **`cache_tool clear`** 가 동작한다
  (`cmd_list` 는 애초에 언어 코드를 검증하지 않고 `strcmp` 필터만 하므로 영향 없음 — 교차검토 반영)
- 클라이언트가 `zhs`/`zht` 를 직접 보내는 경우도 3글자 분기에서 통과(**공식 지원 입력**, §3.1)

`LANGUAGE_NAMES` 배열에 추가:
```c
{"zhs", "Chinese (Simplified)"},
{"zht", "Chinese (Traditional)"},
```
- `get_language_name()` 폴백이 올바른 이름을 반환
- 이름으로 요청(`"Chinese (Simplified)"`)해도 정규화됨
- **필수 이유(교차검토 반영)**: legacy 프롬프트 경로의 `src/http_client.c:639` 는
  `snprintf(..., "Translate FROM %s TO %s", get_language_name(from_lang), get_language_name(to_lang))`
  로 **NULL 검사 없이** 결과를 `%s` 에 넘긴다. 이름이 없으면 `(null)` 이 프롬프트에 들어간다.

> **선결 결함(이번 범위 밖, 별도 과제)**: 위 `http_client.c:639` 는 `zhs`/`zht` 와 무관하게
> 이미 결함이다. `LANGUAGE_NAMES` 는 20개뿐이라 `swe`(Swedish), `fin`(Finnish) 등
> **ISO 배열에는 있으나 이름이 없는 코드**가 legacy 프롬프트로 요청되면 `(null)` 이 들어간다.
> 표준상 UB 이며 glibc 에서만 `(null)` 로 출력된다. NULL 폴백을 추가해야 한다.

### 5.2 `language_tables.conf`

`[CODE_MAPPING]` 섹션에 추가 — 파서 정규식 `^([a-zA-Z]{3}):([a-zA-Z]{2,3})$` 를 만족.
**반드시 `chi:zh`(171행) 바로 뒤, "Language Code to Name" 주석(175행) 앞**에 넣는다:
```
zhs:zh
zht:zh
```

> **순서가 중요한 이유(교차검토 반영)**: `language_table_to_iso639_2()` 는 `iso639_1` 로 **첫 일치**를
> 반환한다(`src/language_table.c:489-500`). `zhs:zh` 를 `chi:zh` 앞에 두면 `"zh"` 의 역변환이
> `chi` → `zhs` 로 바뀐다. 이 역변환은 `language_table_resolve()` 의 2글자/지역변형 분기에서만 쓰이고
> (`language_table.c:532,547`), 정규화가 먼저 일어나는 HTTP 경로에서는 **도달하지 않으므로 실제 영향은
> 없다**. 그래도 모호함을 남기지 않기 위해 순서를 고정한다.

언어명 섹션(1220행 `# Chinese` 블록)에 추가 — 파서 정규식 `^([a-zA-Z0-9-]+)=(["']?)(.+?)\2$` 를 만족:
```
zhs="Chinese (Simplified)"
zht="Chinese (Traditional)"
```

오타/정확도 수정:
```
zh-CH="Chinese"              →  zh-CN="Chinese (Simplified)"
zh-TW="Chinese"              →  zh-TW="Chinese (Traditional)"
```

이 설정으로 `language_table_resolve("zhs")` 는 name=`Chinese (Simplified)`, iso639_1=`zh`, iso639_2=`zhs` 를 반환하고,
프롬프트에는 `{TARGET_LANG}=Chinese (Simplified)`, `{TARGET_CODE}=zh` 가 들어간다.
**§2.3 실측대로 스크립트는 `{TARGET_LANG}` 이름이 결정하므로 `{TARGET_CODE}` 가 `zh` 인 것은 문제가 되지 않는다.**

> **삽입 위치를 틀리면 조용히 실패한다(교차검토 반영)**: 이름 섹션에 `zhs:zh` 를 넣으면
> `parse_language_line()` 실패 후 `LOG_DEBUG` 만 남기고 무시되고(`language_table.c:333`, 릴리스 빌드에서는
> 출력조차 안 됨), 매핑 섹션에 `zhs="..."` 를 넣으면 아무 로그 없이 버려진다(`language_table.c:295-327`).
>
> **이름 엔트리 누락도 조용히 잘못 동작한다(자체 점검)**: `language_table_resolve()` 3글자 분기는
> `lookup(code)` 가 실패하면 `lookup(iso639_1)` 로 폴백한다(`language_table.c:525-528`). 즉
> `zhs:zh` 매핑만 넣고 `zhs="Chinese (Simplified)"` 를 빠뜨리면 이름이 `Chinese` 로 조용히 대체되어
> **번체 요청이 간체로 번역된다**. 두 줄은 반드시 함께 넣고 §7.2 통합 테스트로 확인한다.

### 5.3 문구·주석 정정 (교차검토 반영)

`zhs`/`zht` 는 ISO 639-2 코드가 아니므로 다음 서술을 "지원되는 3글자 내부 코드"로 정정한다:

| 위치 | 현재 문구 |
|---|---|
| `include/utils.h:22,31,34` | "ISO 639-2 language code validation" 등 |
| `include/trans_cache.h:16-17` | `/* ISO 639-2 language code */` |
| `cache_sqlite_schema.sql:9-10,19-20` | "ISO 639-2 source language code (3 chars)" |
| `src/cache_tool.c:141` | `"Error: Invalid language code (must be ISO 639-2)"` |

### 5.4 변경하지 않는 것 (의도적)

| 대상 | 이유 |
|---|---|
| `include/json_handler.h`, `include/trans_cache.h` 의 `[4]` 버퍼 | 정규화 결과가 항상 3글자 |
| SQLite 스키마 `CHECK(length=3)`, 기존 캐시 DB | 동일 |
| `src/json_handler.c` | `normalize_language_code()` 호출부 그대로 |
| `cache_tool` 의 백엔드 선택 | `cache_tool` 은 `migrate` 외 모든 명령에서 **text 백엔드만** 연다(`cache_tool.c:811` → `trans_cache.c:80`). SQLite 캐시는 `cache_tool` 로 조작할 수 없다(교차검토 반영) |
| 응답 스키마 | 언어 코드가 응답에 없음 |
| `PROMPT_PREFIX*.txt` | 언어명 플레이스홀더가 이미 존재 |

---

## 6. 하위 호환성

| 항목 | 영향 |
|---|---|
| 기존 `chi` / `"Chinese"` 요청 | **변화 없음.** 정규화 결과도 `chi`, 프롬프트 이름도 `Chinese` → 기존 캐시 항목 그대로 재사용 |
| 기존 캐시 DB(sqlite/text) | 스키마·내용 변경 없음. `zhs`/`zht` 는 새 행으로만 추가됨 |
| 기존 클라이언트 | 응답 스키마 불변, 기존에 200 이던 요청은 계속 200 |
| 새로 200 이 되는 요청 | **BCP-47 태그**: `zh-Hans`, `zh-Hant`(+ 유효한 하위 태그), `zh-CN`, `zh-TW`, `zh-HK`, `zh-MO`, `zh-SG`, `zh-MY`, `zh` / **3글자 코드**: `zho`, `zhs`, `zht` / **언어명**: `Chinese (Simplified)`, `Chinese (Traditional)` |
| 계속 422 인 요청 | `cmn`, `yue`, `zh-yue`, `zh-latn`, 그 밖의 미등록 코드 |

---

## 7. 테스트 계획

### 7.1 단위 (정규화 함수)

| 입력 | 기대 |
|---|---|
| `zh-Hans`, `zh-hans`, `ZH-HANS`, `zh_Hans` | `zhs` |
| `zh-Hans-HK`, `zh-Hans-SG` | `zhs` |
| `zh-Hant`, `ZH-HANT`, `zh-Hant-TW`, `zh_Hant` | `zht` |
| `zh-TW`, `zh-HK`, `zh-MO` | `zht` |
| `zh-CN`, `zh-SG`, `zh-MY` | `zhs` |
| `zh`, `zho`, `ZH` | `chi` |
| `chi`, `CHI` | `chi` (기존과 동일) |
| `zhs`, `zht` | 그대로 |
| `"Chinese (Simplified)"` / `"Chinese (Traditional)"` | `zhs` / `zht` |
| `zh-`, `zh--hans`, `zhx`, `zh-hansx` | NULL (422) — 경계 검증 |
| `zh-Hans-`, `zh-Hans--CN`, `zh-Hans-中文`, `zh-Hans-!!!`, `zh-hans-verylongsubtag` | NULL (422) — **서브태그 문법 검증** (교차검토 반영) |
| `zh-Hant-CN`, `zh-TW-Hans`, `zh-CN-Hant` | 각각 `zht`, `zhs`, `zht` — **스크립트 우선 규칙 검증** |
| `zh-xx`, `zh-latn` | NULL (422) — 미지의 서브태그 |
| 10,000자 문자열, 빈 문자열, `中文`, `"chi "` | NULL (422) — 기존 동작 유지 |

### 7.2 통합 (API)

1. `to=zh-Hans` → 200, 결과가 **간체**(`会议`류)
2. `to=zh-Hant` → 200, 결과가 **번체**(`會議`류)
3. 같은 원문·같은 `from` 으로 `zh-Hans` / `zh-Hant` 두 번 요청 → **서로 다른 번역문**, 캐시에 **두 개의 행**(`to_lang` 이 `zhs`/`zht`)
4. `to=chi` → 기존 캐시 행에 히트(재번역 없음, 응답 0.0x초)
5. `to=zh-Hant` 재요청 → 캐시 히트
6. `to=cmn` → 422 유지
7. 역방향 `from=zh-Hant, to=kor` → 200
8. **`to=zht` (3글자 직접 입력)** → 200, 번체. `to=zh-Hant` 요청과 **같은 캐시 행에 히트**(정규화 결과가 동일하므로)
9. **`to=zhs`** → 200, 간체. `to=zh-Hans` 와 같은 캐시 행

### 7.3 회귀

- 기존 `kor↔eng`, `jpn→kor` 정상
- 동시 200요청 무크래시 (캐시 잠금 회귀 확인)
- valgrind: 누수 0, 오류 0
- `make` / `make debug` 경고 0
- `cache_tool list kor zhs` 가 동작(⑤ 확인)

---

## 8. 리스크와 완화

| 리스크 | 영향 | 완화 |
|---|---|---|
| `zhs`/`zht` 는 ISO 639-2 표준 코드가 아님 | 외부 도구·운영자가 표준 코드로 오인 | README API 문서에 **transbasket 전용 3글자 코드**로 명시하고, 표준 태그(`zh-Hans`/`zh-Hant`)를 권장 표기로 안내 |
| 나중에 Option B 로 전환 시 | 캐시 이관이 **단순 UPDATE 로 불가능** | 아래 절차 필요(교차검토 반영) |
| 접두 매칭 오판 (`zh-hansx` 등) | 잘못된 스크립트로 번역 | 다음 문자가 `'-'` 인지 확인하는 경계 검사 + 단위 테스트로 고정 |
| `zh` → `chi` 선택이 클라이언트 기대와 다를 수 있음 | 스크립트 미지정 시 모델 기본값(간체) | 문서에 명시. 확정이 필요하면 클라이언트가 `zh-Hans` 를 보내도록 안내 |
| 모델이 지시를 무시하고 다른 스크립트 산출 | 결과 품질 | 서버 범위 밖(비결정성). 이미 문서화된 한계(README "한계와 보장") |
| `language_tables.conf` 삽입 위치/누락 실수 | 조용히 무시되거나 번체가 간체로 번역됨 | §5.2 경고 + §7.2 통합 테스트로 강제 확인 |

**Option B 전환 시 캐시 이관 절차 (초안의 단순 UPDATE 는 틀렸음)**

초안은 `UPDATE trans_cache SET to_lang='zh-Hant' WHERE to_lang='zht'` 로 이관 가능하다고 적었으나
실측 결과 **`CHECK constraint failed: length(to_lang) = 3`** 으로 실패한다. 또한 캐시 키는
`SHA-256("from|to|text")` 이므로(`src/trans_cache.c:34-38`) 언어 열만 바꾸면 `hash` 열이 어긋나
그 행은 영원히 조회되지 않는다. 올바른 순서는:

1. 스키마 완화(새 테이블 생성 + 데이터 복사, `CHECK` 를 길이 제한 없이 재정의)
2. `from_lang`/`to_lang` **양쪽** 을 `zhs`→`zh-Hans`, `zht`→`zh-Hant` 로 변환
3. 각 행의 `hash` 를 새 코드 기준으로 **재계산**
4. `hash` UNIQUE 충돌 시 `count` 합산 등 병합 규칙 적용

---

## 9. 롤백

`src/utils.c`, `language_tables.conf` 두 파일을 되돌리고 재빌드하면 코드 동작은 완전히 복구된다.
`zhs`/`zht` 요청은 다시 422 가 되고, 기존 `chi` 캐시는 영향받지 않는다.

**캐시에 남은 `zhs`/`zht` 행 정리는 롤백 *전에* 해야 한다(교차검토 반영).**

- 롤백 후에는 `zhs`/`zht` 가 `ISO_639_2_CODES` 에서 사라지므로 `cache_tool clear kor zhs` 가
  `validate_language_code()` 단계에서 거부된다(`src/cache_tool.c:140-142`).
- 애초에 `cache_tool` 은 `migrate` 를 제외한 모든 명령에서 **text 백엔드만** 연다
  (`cache_tool.c:811` → `trans_cache.c:80`). 운영 캐시가 SQLite 라면 `cache_tool` 로는 정리할 수 없다.

따라서 정리 방법은 백엔드별로 다음과 같다:

| 백엔드 | 정리 방법 |
|---|---|
| SQLite | `sqlite3 <db> "DELETE FROM trans_cache WHERE from_lang IN ('zhs','zht') OR to_lang IN ('zhs','zht');"` |
| text (JSONL) | 롤백 **전에** `cache_tool clear <from> zhs` / `zht` 실행, 또는 해당 JSON 라인을 직접 제거 |

정리하지 않고 두어도 조회되지 않을 뿐 오동작하지는 않는다(캐시 키가 달라 히트가 발생하지 않음).

---

## 10. 작업 체크리스트

- [x] `src/utils.c`: `fold_lang_tag()`(길이 가드 + `(unsigned char)` 캐스팅), `normalize_chinese_tag()`(서브태그 분해 + 문법 검증 + 스크립트 우선) 추가, `normalize_language_code()` 진입부 수정
- [x] `src/utils.c`: `ISO_639_2_CODES` 에 `zhs`/`zht`, `LANGUAGE_NAMES` 에 이름 2건 추가
- [x] `language_tables.conf`: `[CODE_MAPPING]` 2줄(**`chi:zh` 뒤**), 언어명 2줄(**둘 다 필수**), `zh-CH`/`zh-TW` 정정
- [x] `include/utils.h`, `include/trans_cache.h`, `cache_sqlite_schema.sql`, `src/cache_tool.c:141` 의 "ISO 639-2" 문구 정정
- [x] 단위 테스트(§7.1) 통과
- [x] 통합 테스트(§7.2) 통과 — 간체/번체 실제 산출물 확인
- [x] 회귀 테스트(§7.3) 통과
- [x] `README.md`: `POST /translate` 의 `from`/`to` 설명에 **지원 입력 형식 3종**(BCP-47 태그 / 3글자 코드 `zhs`·`zht` / 언어명)과 정규화 결과표 추가
- [x] `CLAUDE.md` "Adding New Language Support" 에 중국어 스크립트 처리 규칙 추가
- [x] `TEST_RESULTS.md` 에 검증 결과 기록
- [x] (별도 과제) `src/http_client.c:639` 의 `get_language_name()` NULL 미검사 수정 — `swe`/`fin` 등 이름 없는 코드에서 프롬프트에 `(null)` 이 들어가는 선결 결함

---

## 부록 A. 실측 증적 (2026-08-21)

교차검토에서 "C 코드만으로는 검증할 수 없다"고 지적된 항목의 근거다.

### A.1 모델의 간체/번체 구분 (codex mcp-server 직접 호출)

원문: `안녕하세요. 오늘 회의는 오후 3시에 시작하며, 자료는 미리 준비해 주세요.`

| `{TARGET_LANG}` | `{TARGET_CODE}` | 결과 |
|---|---|---|
| `Chinese (Simplified)` | `zh-Hans` | 您好。今天的会议于下午3点开始，请提前准备好资料。 |
| `Chinese (Traditional)` | `zh-Hant` | 您好。今天的會議將於下午3點開始，請提前準備好資料。 |
| `Chinese (Traditional)` | `zh` | 您好。今天的會議將於下午3點開始，請事先準備好資料。 |
| `Chinese (Traditional)` | `zht` | 您好。今天的會議將於下午3點開始，請事先準備好資料。 |
| `Chinese (Simplified)` | `zh` | 您好。今天的会议于下午3点开始，请提前准备好资料。 |
| `Chinese` (스크립트 미지정) | `zh` | 您好。今天的会议将于下午3点开始，请提前准备好资料。 (간체) |

→ **스크립트는 `{TARGET_LANG}` 이름이 결정하며 `{TARGET_CODE}` 값과 무관**하다. 스크립트 미지정 시 간체가 나온다.

### A.2 현재 서버의 코드 수용 여부 (HTTP 실측)

`zh-Hans`, `zh-hans`, `ZH-HANT`, `zh-Hant-TW`, `zh`, `zho`, `cmn`, `zh-CN`, `zh-TW`,
`"Chinese (Simplified)"` → 전부 **422**. `chi`, `CHI`, `"Chinese"` → 200(간체).

### A.3 언어 테이블 해석 (`language_table_resolve` 직접 호출)

| 입력 | name | iso639_1 | iso639_2 | rc |
|---|---|---|---|---|
| `zh-Hans` | Chinese (Simplified) | zh | chi | 0 |
| `zh-Hant` | Chinese (Traditional) | zh | chi | 0 |
| `zh-Hant-HK` | Chinese (Traditional) | zh | chi | 0 |
| `chi` / `zh` / `zh-TW` | Chinese | zh | chi | 0 |
| `zho` / `cmn` / `zh-CN` | (null) | (null) | (null) | -1 |

### A.4 캐시 스키마 제약 (SQLite 실측)

- 기존 DB 에 `to_lang='zh-Hans'`(7글자) INSERT/UPDATE → `CHECK constraint failed: length(to_lang) = 3`
- `to_lang='zhs'`(3글자) INSERT → 통과

### A.5 초안 정규화 로직의 결함 (프로토타입 실측)

접두 매칭 + `'-'` 경계 규칙만 적용했을 때:

| 입력 | 초안 결과 | 판정 |
|---|---|---|
| `zh-Hans-`, `zh-Hans--CN`, `zh-Hans-中文`, `zh-Hans-!!!`, `zh-hans-999` | `zhs` | ✗ 문법 위반인데 통과 |
| `zh-Hant-CN` | `zht` | ✓ |
| `zh-CN-Hant` | `zhs` | ✗ 스크립트와 모순 |

→ §4/§5.1 의 서브태그 분해 + 스크립트 우선 규칙으로 교체함.

### A.6 개정 알고리즘 프로토타입 검증

§5.1 의 서브태그 분해 + 문법 검증 + 스크립트 우선 알고리즘을 독립 프로토타입으로 구현해 37개 케이스를 실행한 결과
**불일치 0건**. 교차검토에서 지적된 케이스가 모두 의도대로 처리된다:

| 입력 | 결과 |
|---|---|
| `zh-Hans-`, `zh-Hans--CN`, `zh-Hans-中文`, `zh-Hans-!!!` | (null) → 422 |
| `zh-CN-Hant`, `zh-Hant-CN` | `zht` (스크립트 우선) |
| `zh-TW-Hans` | `zhs` (스크립트 우선) |
| `zh-xx`, `zh-latn`, `zh-verylongsubtag` | (null) → 422 |
| `chi`, `cmn` | (null) — 중국어 핸들러를 통과하지 않고 기존 3글자 경로로 넘어감(설계대로) |

---

## 11. 교차검토 판정 내역 (codex)

| # | codex 주장 | 검증 방법 | 판정 |
|---|---|---|---|
| 1 | 잘못된 태그(`zh-Hans-`, `zh-Hans--CN`, `zh-Hans-中文`)가 `zhs` 로 통과 | 프로토타입 실행 | ✅채택 — §4/§5.1 알고리즘 교체 |
| 2 | `tolower()` 인자에 `(unsigned char)` 캐스팅 필요 | C 표준 확인 | ✅채택 — §5.1 명시 |
| 3 | 모순 태그 정책 부재(`zh-CN-Hant`→`zhs`) | 프로토타입 실행 | ✅채택 — 스크립트 우선 규칙 명문화 |
| 4 | Option B 이관 SQL 이 `CHECK` 에 걸리고 해시 재계산도 필요 | 실제 `UPDATE` 실행 → `CHECK constraint failed` 확인, 해시 입력 코드 확인 | ✅채택 — §8 절차 재작성 |
| 5 | 롤백 절차가 성립하지 않음 | `cache_tool.c:140-142` 검증 로직 확인 | ✅채택 — §9 재작성 |
| 6 | `cache_tool list` 는 검증하지 않음, `clear` 만 함 | `cmd_list`/`cmd_clear` 코드 확인 | ✅채택 — §5.1 문구 정정 |
| 7 | `cache_tool` 은 항상 text 백엔드만 연다 | `cache_tool.c:811` → `trans_cache.c:80` 확인 | ✅채택 — §5.3/§9 반영 |
| 8 | `http_client.c:639` 가 `get_language_name()` 을 NULL 검사 없이 사용 | 해당 행 확인 + 전수 grep + `LANGUAGE_NAMES` 20개 대조(`swe`/`fin` 누락 확인) | ✅채택 — 선결 결함으로 별도 과제 등록 |
| 9 | `zhs:zh` 추가 시 `to_iso639_2("zh")` 역변환이 모호해짐 | 함수 코드 + 전수 사용처 조사(`language_table.c:532,547` 만 사용) | ⚖️부분채택 — 사실이나 정규화 이후 경로에서 **도달 불가**. 순서 고정만 반영 |
| 10 | "API 표면에 드러나지 않는다"는 자기모순 | 계획서 73/97/271행 대조 | ✅채택 — 사실관계 정정. 이후 사용자 결정으로 `zhs`/`zht` 를 **공식 지원 입력**으로 승격(비문서화 별칭 아님) |
| 11 | ISO 배열 188개·이름 20개 서술은 정확 | 직접 카운트 | ✅확인 — 조치 없음 |
| 12 | conf 삽입 위치가 틀리면 조용히 무시됨 | 파서 정규식·섹션 경계·실패 처리 코드 확인 | ✅채택 — §5.2 경고 추가 |
| 13 | 실측 주장은 코드로 검증 불가하므로 증적 첨부 필요 | — | ✅채택 — 부록 A 추가 |
| 14 | 헤더/스키마 주석·`cache_tool` 오류 문구 정정 필요 | grep 으로 문구 실재 확인 | ✅채택 — §5.3 추가 |
| 15 | "기존 `chi` 요청 동일" 주장은 성립 | 코드 경로 확인 | ✅확인 — 조치 없음 |

자체 점검으로 추가 발견(교차검토에는 없던 항목): `language_tables.conf` 에 `zhs:zh` 매핑만 넣고
`zhs="Chinese (Simplified)"` 이름 엔트리를 빠뜨리면 `language_table_resolve()` 가 `lookup("zh")` 로
폴백해 이름이 `Chinese` 가 되고 **번체 요청이 조용히 간체로 번역된다**(§5.2 에 반영).
