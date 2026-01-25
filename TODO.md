# TranslateGemma 프롬프트 포맷 지원 작업 계획

## 개요

TranslateGemma 모델의 프롬프트 포맷을 지원하기 위한 개선 작업입니다.

### 현재 상태

1. **언어 코드 시스템**
   - `transbasket_curl_sample.txt`의 from/to: ISO 639-2 3글자 코드 사용 (`kor`, `eng`, `jpn`)
   - `language_tables.conf`: ISO 639-1 2글자 코드만 존재 (`ko`, `en`, `ja`)
   - `utils.c`: 하드코딩된 ISO 639-2 코드 배열과 제한된 언어 이름 매핑

2. **프롬프트 시스템**
   - 현재: `PROMPT_PREFIX.txt`에서 `<source></source>` 태그 방식 사용
   - `http_client.c`의 `build_instruction_message()`: `[TARGET LANGUAGE]`, `{{LANGUAGE_TO}}` 치환

3. **TranslateGemma 요구사항**
   ```
   You are a professional {SOURCE_LANG} ({SOURCE_CODE}) to {TARGET_LANG} ({TARGET_CODE}) translator.
   Your goal is to accurately convey the meaning and nuances of the original {SOURCE_LANG} text
   while adhering to {TARGET_LANG} grammar, vocabulary, and cultural sensitivities.
   Produce only the {TARGET_LANG} translation, without any additional explanations or commentary.
   Please translate the following {SOURCE_LANG} text into {TARGET_LANG}:


   {TEXT}
   ```
   - 주의: 텍스트 앞에 **빈 줄 2개** 필요

---

## 작업 항목

### Phase 1: language_tables.conf 확장

#### 1.1 파일 포맷 변경
**파일**: `language_tables.conf`

현재:
```conf
ko="Korean"
en="English"
```

변경:
```conf
# Korean
ko="Korean"
kor="Korean"       # ISO 639-2 alias

# English
en="English"
eng="English"      # ISO 639-2 alias
```

**작업 내용**:
- 주요 언어에 대해 ISO 639-2 3글자 코드 추가
- 기존 2글자 코드와 동일한 언어명으로 매핑
- 최소 지원 언어: kor, eng, jpn, chi, spa, fre, ger, rus, ara, por, ita, dut, pol, tur, vie, tha, ind

#### 1.2 코드 매핑 테이블 추가
**파일**: `language_tables.conf`

상단에 별도 섹션 추가:
```conf
# ===========================================
# ISO 639-2 to ISO 639-1 Code Mapping
# Format: [ISO639-2]=[ISO639-1]
# ===========================================
[CODE_MAPPING]
kor=ko
eng=en
jpn=ja
chi=zh
spa=es
fre=fr
ger=de
rus=ru
ara=ar
por=pt
ita=it
dut=nl
pol=pl
tur=tr
vie=vi
tha=th
ind=id
may=ms
hin=hi
ben=bn
```

---

### Phase 2: language_table 모듈 확장

#### 2.1 데이터 구조 확장
**파일**: `include/language_table.h`

```c
/* 코드 매핑 엔트리 */
typedef struct {
    char *iso639_2;    /* 3글자 코드 (kor, eng, jpn) */
    char *iso639_1;    /* 2글자 코드 (ko, en, ja) */
} CodeMapping;

/* LanguageTable 구조체에 추가 */
typedef struct {
    LanguageEntry *entries;
    size_t count;
    size_t capacity;

    /* 코드 매핑 (신규) */
    CodeMapping *code_mappings;
    size_t mapping_count;
    size_t mapping_capacity;
} LanguageTable;
```

#### 2.2 새 API 함수 추가
**파일**: `include/language_table.h`

```c
/**
 * ISO 639-2 코드를 ISO 639-1 코드로 변환
 * @param table 언어 테이블
 * @param iso639_2 3글자 코드 (예: "kor")
 * @return 2글자 코드 (예: "ko") 또는 NULL
 */
const char *language_table_to_iso639_1(const LanguageTable *table, const char *iso639_2);

/**
 * ISO 639-1 코드를 ISO 639-2 코드로 변환
 * @param table 언어 테이블
 * @param iso639_1 2글자 코드 (예: "ko")
 * @return 3글자 코드 (예: "kor") 또는 NULL
 */
const char *language_table_to_iso639_2(const LanguageTable *table, const char *iso639_1);

/**
 * 임의의 코드(2글자 또는 3글자)에서 언어 정보 조회
 * @param table 언어 테이블
 * @param code 언어 코드 (2글자 또는 3글자)
 * @param out_name 언어 이름 출력 (Korean, English 등)
 * @param out_iso639_1 ISO 639-1 코드 출력 (ko, en 등)
 * @param out_iso639_2 ISO 639-2 코드 출력 (kor, eng 등)
 * @return 0 성공, -1 실패
 */
int language_table_resolve(const LanguageTable *table, const char *code,
                          const char **out_name,
                          const char **out_iso639_1,
                          const char **out_iso639_2);
```

#### 2.3 구현 수정
**파일**: `src/language_table.c`

- `[CODE_MAPPING]` 섹션 파싱 로직 추가
- 새 API 함수 구현
- 기존 lookup 함수가 2글자/3글자 코드 모두 처리하도록 수정

---

### Phase 3: 설정 파일 확장

#### 3.1 transbasket.conf 설정 추가
**파일**: `transbasket.conf`

```conf
# Prompt configuration
# PROMPT_PREFIX_FILE: Path to prompt prefix file (default: PROMPT_PREFIX.txt)
# Available templates:
#   - PROMPT_PREFIX.txt (default, uses <source> tags)
#   - PROMPT_PREFIX_translategemma.txt (TranslateGemma format)
PROMPT_PREFIX_FILE="PROMPT_PREFIX.txt"

# LANGUAGE_TABLE_FILE: Path to language table configuration
LANGUAGE_TABLE_FILE="language_tables.conf"
```

#### 3.2 Config 구조체 확장
**파일**: `include/config_loader.h`

```c
typedef struct {
    /* 기존 필드들 ... */

    /* Prompt configuration (신규) */
    char *prompt_prefix_file;     /* PROMPT_PREFIX 파일 경로 */
    char *language_table_file;    /* 언어 테이블 파일 경로 */
} Config;
```

#### 3.3 config_loader.c 수정
**파일**: `src/config_loader.c`

- `PROMPT_PREFIX_FILE` 파싱 추가
- `LANGUAGE_TABLE_FILE` 파싱 추가
- 기본값: `PROMPT_PREFIX.txt`, `language_tables.conf`

---

### Phase 4: TranslateGemma 프롬프트 템플릿 생성

#### 4.1 새 프롬프트 파일 생성
**파일**: `PROMPT_PREFIX_translategemma.txt`

```
You are a professional {SOURCE_LANG} ({SOURCE_CODE}) to {TARGET_LANG} ({TARGET_CODE}) translator. Your goal is to accurately convey the meaning and nuances of the original {SOURCE_LANG} text while adhering to {TARGET_LANG} grammar, vocabulary, and cultural sensitivities. Produce only the {TARGET_LANG} translation, without any additional explanations or commentary. Please translate the following {SOURCE_LANG} text into {TARGET_LANG}:


{TEXT}
```

**주의사항**:
- `{TEXT}` 앞에 빈 줄 2개 필수
- 플레이스홀더: `{SOURCE_LANG}`, `{SOURCE_CODE}`, `{TARGET_LANG}`, `{TARGET_CODE}`, `{TEXT}`

---

### Phase 5: http_client.c 프롬프트 빌더 개선

#### 5.1 플레이스홀더 치환 함수 수정
**파일**: `src/http_client.c`

`build_instruction_message()` 함수 개선:

```c
static char *build_instruction_message(OpenAITranslator *translator,
                                       const char *from_lang,
                                       const char *to_lang,
                                       const char *text) {
    /* 언어 테이블에서 정보 조회 */
    const char *source_name, *source_iso1, *source_iso2;
    const char *target_name, *target_iso1, *target_iso2;

    language_table_resolve(translator->lang_table, from_lang,
                          &source_name, &source_iso1, &source_iso2);
    language_table_resolve(translator->lang_table, to_lang,
                          &target_name, &target_iso1, &target_iso2);

    char *result = strdup(translator->config->prompt_prefix);

    /* TranslateGemma 포맷 플레이스홀더 */
    result = str_replace_all(result, "{SOURCE_LANG}", source_name);
    result = str_replace_all(result, "{SOURCE_CODE}", source_iso1);
    result = str_replace_all(result, "{TARGET_LANG}", target_name);
    result = str_replace_all(result, "{TARGET_CODE}", target_iso1);
    result = str_replace_all(result, "{TEXT}", text);

    /* 기존 포맷 호환성 */
    result = str_replace_all(result, "[TARGET LANGUAGE]", target_name);
    result = str_replace_all(result, "{{LANGUAGE_TO}}", target_name);
    result = str_replace_all(result, "{{LANGUAGE_BASE}}", source_name);

    return result;
}
```

#### 5.2 OpenAITranslator 구조체 확장
**파일**: `include/http_client.h`

```c
typedef struct {
    Config *config;
    LanguageTable *lang_table;  /* 언어 테이블 참조 (신규) */
    int max_retries;
    int timeout;
} OpenAITranslator;
```

#### 5.3 메시지 구조 변경 (TranslateGemma 모드)
**파일**: `src/http_client.c`

TranslateGemma 프롬프트 감지 시:
- `{TEXT}` 플레이스홀더가 있으면 단일 메시지로 전송
- 기존 `<source>` 태그 방식이면 기존 4-메시지 구조 유지

```c
/* TranslateGemma 포맷 감지 */
bool is_translategemma_format = strstr(translator->config->prompt_prefix, "{TEXT}") != NULL;

if (is_translategemma_format) {
    /* 단일 user 메시지로 전송 */
    cJSON *user_message = cJSON_CreateObject();
    cJSON_AddStringToObject(user_message, "role", "user");
    cJSON_AddStringToObject(user_message, "content", instruction);
    cJSON_AddItemToArray(messages, user_message);
} else {
    /* 기존 4-메시지 구조 유지 */
    /* ... */
}
```

---

### Phase 6: 통합 및 초기화

#### 6.1 main.c 수정
**파일**: `src/main.c`

```c
/* 언어 테이블 로드 */
LanguageTable *lang_table = language_table_load(config->language_table_file);
if (!lang_table) {
    LOG_INFO("Warning: Failed to load language table, using built-in defaults");
}

/* 번역기 초기화 시 언어 테이블 전달 */
OpenAITranslator *translator = openai_translator_init(config, lang_table, max_retries, timeout);
```

#### 6.2 openai_translator_init 시그니처 변경
**파일**: `src/http_client.c`

```c
OpenAITranslator *openai_translator_init(Config *config,
                                         LanguageTable *lang_table,
                                         int max_retries,
                                         int timeout);
```

---

## 작업 순서

| 순서 | Phase | 작업 내용 | 예상 변경 파일 |
|------|-------|----------|---------------|
| 1 | 1.1 | language_tables.conf에 3글자 코드 추가 | language_tables.conf |
| 2 | 1.2 | 코드 매핑 섹션 추가 | language_tables.conf |
| 3 | 2.1-2.3 | language_table 모듈 확장 | language_table.h, language_table.c |
| 4 | 3.1-3.3 | 설정 파일 확장 | transbasket.conf, config_loader.h, config_loader.c |
| 5 | 4.1 | TranslateGemma 프롬프트 템플릿 생성 | PROMPT_PREFIX_translategemma.txt |
| 6 | 5.1-5.3 | http_client.c 프롬프트 빌더 개선 | http_client.h, http_client.c |
| 7 | 6.1-6.2 | 통합 및 초기화 | main.c, http_client.c |
| 8 | - | 테스트 및 검증 | - |

---

## 호환성 고려사항

1. **기존 API 호환성 유지**
   - from/to 필드는 여전히 3글자 ISO 639-2 코드 사용
   - 기존 PROMPT_PREFIX.txt 형식 계속 지원

2. **설정 파일 기본값**
   - PROMPT_PREFIX_FILE 미지정 시: PROMPT_PREFIX.txt 사용
   - LANGUAGE_TABLE_FILE 미지정 시: language_tables.conf 사용

3. **언어 테이블 로드 실패 시**
   - 기존 utils.c의 하드코딩된 매핑 사용 (fallback)

---

## 테스트 계획

1. **언어 코드 변환 테스트**
   ```bash
   # 3글자 -> 2글자
   kor -> ko
   eng -> en

   # 2글자 -> 언어명
   ko -> Korean
   en -> English
   ```

2. **프롬프트 생성 테스트**
   ```bash
   # TranslateGemma 포맷
   from=kor, to=eng
   -> "You are a professional Korean (ko) to English (en) translator..."
   ```

3. **통합 테스트**
   ```bash
   curl -X POST http://localhost:8889/translate \
     -H "Content-Type: application/json" \
     -d '{
       "timestamp": "2025-01-25T00:00:00.000Z",
       "uuid": "550e8400-e29b-41d4-a716-446655440000",
       "from": "kor",
       "to": "eng",
       "text": "안녕하세요"
     }'
   ```

---

## 완료 기준

- [x] language_tables.conf에 ISO 639-2 코드 추가됨 (740 entries, 159 code mappings)
- [x] language_table 모듈이 2글자/3글자 코드 모두 지원
- [x] transbasket.conf에서 프롬프트 파일 지정 가능 (PROMPT_PREFIX_FILE, LANGUAGE_TABLE_FILE)
- [x] PROMPT_PREFIX_translategemma.txt 생성됨
- [x] 플레이스홀더 치환 정상 동작 ({SOURCE_LANG}, {SOURCE_CODE}, {TARGET_LANG}, {TARGET_CODE}, {TEXT})
- [x] 기존 PROMPT_PREFIX.txt 방식과 호환성 유지
- [x] 테스트 통과 (2026-01-25 검증 완료)
