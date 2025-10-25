# Transbasket 개발 TODO

## 프로젝트 현황

**현재 상태**: Production C implementation (Python POC 건너뜀)

### 구현 완료된 핵심 기능 ✓
- [x] API 계약 및 스키마 설계 (RFC 3339, UUID v4, ISO 639-2)
- [x] 설정 파일 파서 (`config_loader.c`) - KEY="value" 형식, PROMPT_PREFIX.txt 로딩
- [x] HTTP 서버 (`http_server.c`) - libmicrohttpd, thread-per-connection 모델
- [x] JSON 처리 (`json_handler.c`) - cJSON 기반 요청/응답 처리
- [x] HTTP 클라이언트 (`http_client.c`) - libcurl 기반 OpenAI API 호출
- [x] 재시도 로직 - 지수 백오프, 최대 3회 재시도
- [x] 유틸리티 (`utils.c`) - UUID/timestamp/language code 검증, emoji 제거
- [x] 로깅 시스템 - 표준화된 로그 형식, trace 디렉토리
- [x] 에러 처리 - HTTP 상태 코드 매핑 (400/422/500/502/503/504)
- [x] 빌드 시스템 - Makefile, debug/release 빌드
- [x] 헬스체크 엔드포인트 (`/health`)
- [x] 번역 캐시 시스템 (`trans_cache.c`) - JSONL 파일 기반, SHA256 해싱
- [x] 캐시 관리 도구 (`cache_tool`) - 통계, 정리, 검색 기능

### API 스키마

요청 JSON:
```json
{
  "timestamp": "RFC3339 string",
  "uuid": "UUID v4 string",
  "from": "language code (ISO 639-2)",
  "to": "language code (ISO 639-2)",
  "text": "string"
}
```

응답 JSON:
```json
{
  "timestamp": "RFC3339 string",
  "uuid": "UUID v4 string",
  "from": "language code",
  "to": "language code",
  "text": "string",
  "translated_text": "string"
}
```

---

## 진행 중인 작업

### 번역 캐시 시스템 개선 (P1) 🔥

**목표**: 다중 백엔드 지원 (text, SQLite, MongoDB, Redis)

**현재 구현 상태**:
- [x] JSONL 파일 기반 캐시 (`trans_cache.c`)
- [x] SHA256 해시 기반 캐시 키
- [x] 사용 빈도 추적 (count) 및 LRU (last_used)
- [x] 자동 정리 기능 (cleanup by days)
- [x] 캐시 도구 (`cache_tool`)

**Phase 1: SQLite 백엔드 구현 (현재 작업)**

#### ✅ 1.1 설정 파일 확장 (완료)
- [x] `transbasket.conf`에 `TRANS_CACHE_TYPE` 추가
  - 지원 값: `text`, `sqlite`, `mongodb`, `redis`
  - 기본값: `text` (하위 호환성)
- [x] SQLite 관련 설정 추가
  - `TRANS_CACHE_SQLITE_PATH`: SQLite DB 파일 경로 (기본: `./trans_cache.db`)
  - `TRANS_CACHE_SQLITE_JOURNAL_MODE`: 저널 모드 (기본: `WAL`)
  - `TRANS_CACHE_SQLITE_SYNC`: 동기화 모드 (기본: `NORMAL`)
- [x] `config_loader.c`에서 새 설정 파싱 및 검증
  - CacheBackendType enum 정의
  - 설정 파싱 로직 구현 (strcasecmp 기반)
  - 유효성 검증 (journal_mode, sync 값 체크)
- [x] `transbasket.test.conf` 생성 (포트 8890, 테스트용)

#### ✅ 1.2 SQLite 스키마 설계 및 파일 생성 (완료)
- [x] `cache_sqlite_schema.sql` 파일 작성
  - 테이블 구조 설계 (현재 JSONL 필드 기반)
  - 5개 인덱스 설계 (hash, lang_pair, last_used, count, lang_hash)
  - CHECK 제약조건 추가 (hash 길이, 언어 코드 길이 등)
  - PRAGMA 최적화 설정 (WAL, NORMAL, cache_size, mmap_size 등)
  - FTS5 전문 검색 인덱스 (주석 처리, 선택적 활성화 가능)

#### ✅ 1.3 캐시 백엔드 추상화 (설계 완료)
- [x] `trans_cache.h` 확장 - 백엔드 추상화 인터페이스
  - `CacheBackendOps` 구조체: 함수 포인터 기반 백엔드 인터페이스
    - `lookup()`: 캐시 조회
    - `add()`: 캐시 추가
    - `update_count()`: 사용 횟수 증가
    - `update_translation()`: 번역 결과 업데이트
    - `save()`: 저장소에 persist
    - `cleanup()`: 오래된 항목 정리
    - `stats()`: 통계 정보 조회
    - `free_backend()`: 백엔드 자원 해제
  - `TransCache` 구조체 재설계
    - `type`: 백엔드 타입 (enum)
    - `backend_ctx`: 백엔드별 컨텍스트 (void*)
    - `ops`: 백엔드 operations (함수 포인터 구조체)
    - `lock`: 스레드 안전성을 위한 rwlock
  - 공개 API 함수 추가
    - `trans_cache_init_with_backend()`: 백엔드 타입 지정 초기화
    - `trans_cache_calculate_hash()`: SHA256 해시 계산 헬퍼
- [x] `cache_backend_text.h` 생성 (헤더 파일)
  - `TextBackendContext` 구조체 정의
  - `text_backend_init()` 선언
  - `text_backend_get_ops()` 선언

#### 🚧 1.3-NEXT 백엔드 리팩토링 (다음 작업) ⚠️

**작업 개요**: 기존 `trans_cache.c`의 JSONL 구현을 새로운 백엔드 아키텍처로 분리

**파일 구조**:
```
src/
├── trans_cache.c          (백엔드 팩토리 + API 래퍼)
├── cache_backend_text.c   (JSONL 백엔드 구현)
└── cache_backend_sqlite.c (SQLite 백엔드 구현 - 새로 작성)

include/
├── trans_cache.h          (공개 API - 이미 수정됨)
├── cache_backend_text.h   (Text 백엔드 헤더 - 이미 생성됨)
└── cache_backend_sqlite.h (SQLite 백엔드 헤더 - 향후 작성)
```

**Step 1: `cache_backend_text.c` 구현**
- [ ] 기존 `trans_cache.c`에서 JSONL 관련 코드 복사
- [ ] `TextBackendContext` 구조체 사용하도록 수정
- [ ] 백엔드 operations 구현:
  ```c
  // 참고: 기존 trans_cache.c의 함수를 다음과 같이 변환
  // trans_cache_lookup() -> text_backend_lookup()
  // trans_cache_add() -> text_backend_add()
  // trans_cache_update_count() -> text_backend_update_count()
  // trans_cache_update_translation() -> text_backend_update_translation()
  // trans_cache_save() -> text_backend_save()
  // trans_cache_cleanup() -> text_backend_cleanup()
  // trans_cache_stats() -> text_backend_stats()
  ```
- [ ] `text_backend_init()` 구현
  - `TransCache` 할당
  - `TextBackendContext` 초기화
  - JSONL 파일 로드
  - rwlock 초기화
  - ops 연결
- [ ] `text_backend_get_ops()` 구현
  - CacheBackendOps 구조체 생성
  - 함수 포인터 매핑
- [ ] SHA256 해시 계산 함수를 public으로 이동
  - `calculate_cache_hash()` → `trans_cache_calculate_hash()`
  - `trans_cache.c`에서 공통 유틸리티로 제공

**Step 2: `trans_cache.c` 리팩토링**
- [ ] 기존 구현 코드 제거 (backend_text.c로 이동했으므로)
- [ ] 백엔드 팩토리 함수 구현:
  ```c
  TransCache *trans_cache_init_with_backend(
      CacheBackendType type,
      const char *config_path,
      void *options
  ) {
      switch (type) {
          case CACHE_BACKEND_TEXT:
              return text_backend_init(config_path);
          case CACHE_BACKEND_SQLITE:
              return sqlite_backend_init(config_path, options);
          // ... 향후 다른 백엔드 추가
          default:
              LOG_INFO("Unknown backend type, using text\n");
              return text_backend_init(config_path);
      }
  }
  ```
- [ ] API 래퍼 함수 구현:
  ```c
  CacheEntry *trans_cache_lookup(TransCache *cache, ...) {
      if (!cache || !cache->ops || !cache->ops->lookup) return NULL;
      pthread_rwlock_rdlock(&cache->lock);
      CacheEntry *result = cache->ops->lookup(cache->backend_ctx, ...);
      pthread_rwlock_unlock(&cache->lock);
      return result;
  }
  // 모든 공개 API에 대해 동일한 패턴 적용
  ```
- [ ] 레거시 호환 함수 유지:
  ```c
  TransCache *trans_cache_init(const char *file_path) {
      return trans_cache_init_with_backend(CACHE_BACKEND_TEXT, file_path, NULL);
  }
  ```
- [ ] SHA256 해시 계산 구현 (공통 유틸리티)

**Step 3: 컴파일 및 테스트**
- [ ] Makefile 업데이트
  ```makefile
  CACHE_OBJS = obj/trans_cache.o obj/cache_backend_text.o
  # ... 나중에 obj/cache_backend_sqlite.o 추가
  ```
- [ ] 컴파일 테스트: `make clean && make`
- [ ] 기존 기능 유지 확인
  ```bash
  # 기존 설정 파일로 테스트 (text 백엔드)
  ./transbasket -c transbasket.test.conf -p PROMPT_PREFIX.txt
  # 포트 8890에서 실행되어야 함
  # 기존 trans_dictionary.test.txt 파일 사용
  ```

#### 🚧 1.4 SQLite 백엔드 구현 (다음 작업)

**작업 개요**: SQLite 데이터베이스 백엔드 구현

**Step 1: 헤더 파일 생성 (`cache_backend_sqlite.h`)**
- [ ] SQLite 백엔드 컨텍스트 구조체 정의:
  ```c
  typedef struct {
      sqlite3 *db;                    // SQLite 데이터베이스 핸들
      char *db_path;                  // DB 파일 경로
      sqlite3_stmt *stmt_lookup;      // Prepared statement: lookup
      sqlite3_stmt *stmt_add;         // Prepared statement: insert
      sqlite3_stmt *stmt_update_count;// Prepared statement: update count
      sqlite3_stmt *stmt_cleanup;     // Prepared statement: cleanup
      // ... 기타 prepared statements
  } SQLiteBackendContext;
  ```
- [ ] 공개 함수 선언:
  ```c
  TransCache *sqlite_backend_init(const char *db_path, void *options);
  CacheBackendOps *sqlite_backend_get_ops(void);
  ```

**Step 2: 백엔드 구현 (`cache_backend_sqlite.c`)**

**2.1 초기화 함수**
- [ ] `sqlite_backend_init()` 구현
  ```c
  TransCache *sqlite_backend_init(const char *db_path, void *options) {
      // 1. TransCache 할당
      // 2. SQLiteBackendContext 초기화
      // 3. sqlite3_open_v2() - FULLMUTEX 모드
      // 4. 스키마 적용 (cache_sqlite_schema.sql 실행)
      // 5. Prepared statements 준비
      // 6. rwlock 초기화
      // 7. ops 연결
  }
  ```
- [ ] 스키마 적용 로직
  ```c
  static int apply_schema(sqlite3 *db, const char *schema_path) {
      // cache_sqlite_schema.sql 파일 읽기
      // sqlite3_exec()로 실행
      // 에러 처리
  }
  ```

**2.2 백엔드 Operations 구현**
- [ ] `sqlite_backend_lookup()` - 캐시 조회
  ```sql
  SELECT * FROM trans_cache WHERE hash = ? LIMIT 1;
  ```
  - Prepared statement 사용
  - 결과를 CacheEntry 구조체로 변환
  - last_used 업데이트 (별도 트랜잭션)

- [ ] `sqlite_backend_add()` - 캐시 추가
  ```sql
  INSERT INTO trans_cache (hash, from_lang, to_lang, source_text,
                           translated_text, count, last_used, created_at)
  VALUES (?, ?, ?, ?, ?, 1, ?, ?)
  ON CONFLICT(hash) DO UPDATE SET
      translated_text = excluded.translated_text,
      count = 1,
      last_used = excluded.last_used;
  ```
  - 해시 충돌 시 업데이트 (덮어쓰기)

- [ ] `sqlite_backend_update_count()` - 사용 횟수 증가
  ```sql
  UPDATE trans_cache
  SET count = count + 1, last_used = ?
  WHERE hash = ?;
  ```

- [ ] `sqlite_backend_update_translation()` - 번역 결과 업데이트
  ```sql
  UPDATE trans_cache
  SET translated_text = ?, count = 1, last_used = ?
  WHERE hash = ?;
  ```

- [ ] `sqlite_backend_save()` - 저장 (WAL 모드에서는 자동)
  ```c
  // WAL 모드에서는 자동 저장
  // PRAGMA wal_checkpoint(PASSIVE); 호출 옵션
  return 0;
  ```

- [ ] `sqlite_backend_cleanup()` - 오래된 항목 삭제
  ```sql
  DELETE FROM trans_cache
  WHERE last_used < ?;  -- threshold = now - days * 86400
  ```

- [ ] `sqlite_backend_stats()` - 통계 조회
  ```sql
  -- 전체 항목 수
  SELECT COUNT(*) FROM trans_cache;

  -- 활성 항목 수 (threshold 이상)
  SELECT COUNT(*) FROM trans_cache WHERE count >= ?;

  -- 만료 항목 수
  SELECT COUNT(*) FROM trans_cache WHERE last_used < ?;
  ```

- [ ] `sqlite_backend_free()` - 자원 해제
  ```c
  // 1. Prepared statements finalize
  // 2. sqlite3_close()
  // 3. 메모리 해제
  ```

**2.3 성능 최적화**
- [ ] Prepared Statements
  - 모든 SQL 쿼리를 prepared statement로 미리 준비
  - `sqlite3_prepare_v2()` 초기화 시 호출
  - 재사용으로 파싱 오버헤드 제거

- [ ] WAL 모드 활성화
  ```c
  sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
  sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
  ```

- [ ] 트랜잭션 배칭 (선택적)
  ```c
  // 대량 삽입 시
  BEGIN TRANSACTION;
  // ... 여러 INSERT
  COMMIT;
  ```

**2.4 스레드 안전성**
- [ ] SQLite 멀티스레드 모드 설정
  ```c
  sqlite3_open_v2(db_path, &db,
                  SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                  SQLITE_OPEN_FULLMUTEX, NULL);
  ```
- [ ] TransCache의 rwlock 활용
  - 읽기 작업: rdlock
  - 쓰기 작업: wrlock

**2.5 에러 처리**
- [ ] SQLite 에러 코드 매핑
  ```c
  if (rc != SQLITE_OK) {
      LOG_INFO("SQLite error: %s\n", sqlite3_errmsg(db));
      return -1;
  }
  ```
- [ ] 로깅 강화
  - SQL 실행 로그 (DEBUG 레벨)
  - 에러 발생 시 상세 정보

**Step 3: Operations 구조체 연결**
- [ ] `sqlite_backend_get_ops()` 구현
  ```c
  CacheBackendOps *sqlite_backend_get_ops(void) {
      static CacheBackendOps ops = {
          .lookup = sqlite_backend_lookup,
          .add = sqlite_backend_add,
          .update_count = sqlite_backend_update_count,
          .update_translation = sqlite_backend_update_translation,
          .save = sqlite_backend_save,
          .cleanup = sqlite_backend_cleanup,
          .stats = sqlite_backend_stats,
          .free_backend = sqlite_backend_free
      };
      return &ops;
  }
  ```

**Step 4: Makefile 업데이트**
- [ ] SQLite 라이브러리 추가
  ```makefile
  LIBS = -lcurl -lmicrohttpd -lcjson -luuid -lm -lssl -lcrypto -lsqlite3
  ```
- [ ] 컴파일 대상 추가
  ```makefile
  CACHE_OBJS = obj/trans_cache.o obj/cache_backend_text.o obj/cache_backend_sqlite.o
  ```
- [ ] `make check-deps`에 SQLite 체크 추가
  ```bash
  pkg-config --exists sqlite3 || echo "sqlite3 not found"
  ```

**Step 5: 테스트**
- [ ] 설정 파일 수정하여 SQLite 백엔드 사용
  ```
  TRANS_CACHE_TYPE="sqlite"
  TRANS_CACHE_SQLITE_PATH="./trans_cache.test.db"
  ```
- [ ] 서버 실행 및 번역 요청 테스트
- [ ] DB 파일 확인: `sqlite3 trans_cache.test.db "SELECT COUNT(*) FROM trans_cache;"`
- [ ] 성능 측정: text vs sqlite 조회 속도 비교

#### 🚧 1.5 JSONL → SQLite 마이그레이션 도구 (향후 작업)

**작업 개요**: 기존 JSONL 캐시를 SQLite로 변환하는 도구

**Step 1: cache_tool 확장**
- [ ] `src/cache_tool.c`에 migrate 명령 추가
  ```bash
  cache_tool migrate --from text --to sqlite \
    --input trans_dictionary.txt \
    --output trans_cache.db
  ```
- [ ] 명령행 파라미터 파싱
  - `--from`: 소스 백엔드 타입
  - `--to`: 대상 백엔드 타입
  - `--input`: 소스 파일/DB 경로
  - `--output`: 대상 파일/DB 경로
  - `--verify`: 마이그레이션 후 검증 활성화 (옵션)

**Step 2: 마이그레이션 로직**
- [ ] JSONL → SQLite 변환 함수
  ```c
  int migrate_text_to_sqlite(const char *input_file, const char *output_db) {
      // 1. Text 백엔드로 JSONL 로드
      TransCache *text_cache = text_backend_init(input_file);

      // 2. SQLite 백엔드 초기화 (새 DB 생성)
      TransCache *sqlite_cache = sqlite_backend_init(output_db, NULL);

      // 3. 트랜잭션 시작
      BEGIN TRANSACTION;

      // 4. 모든 항목 순회 및 복사
      for (size_t i = 0; i < text_cache->size; i++) {
          CacheEntry *entry = text_cache->entries[i];
          sqlite_cache->ops->add(sqlite_cache->backend_ctx, ...);
      }

      // 5. 트랜잭션 커밋
      COMMIT;

      // 6. 검증 (레코드 수 비교)
      if (text_size != sqlite_size) {
          LOG_INFO("Warning: Record count mismatch!\n");
      }
  }
  ```
- [ ] 진행 상황 표시
  ```c
  printf("Migrating: %zu / %zu entries (%.1f%%)\r", i, total, progress);
  ```
- [ ] 에러 처리
  - DB 생성 실패 시 중단
  - 삽입 실패 시 롤백
  - 롤백 실패 시 경고

**Step 3: 역방향 마이그레이션 (SQLite → JSONL)**
- [ ] SQLite → Text 변환 함수 (선택적)
  ```c
  int migrate_sqlite_to_text(const char *input_db, const char *output_file);
  ```

#### 🚧 1.6 통합 및 테스트 (향후 작업)

**작업 개요**: 백엔드 통합 및 종합 테스트

**Step 1: http_server.c 통합**
- [ ] 백엔드 타입에 따른 초기화
  ```c
  // http_server.c의 초기화 부분
  TransCache *cache = trans_cache_init_with_backend(
      config->cache_type,
      config->cache_type == CACHE_BACKEND_TEXT ?
          config->cache_file : config->cache_sqlite_path,
      NULL
  );
  ```
- [ ] 기존 JSONL 캐시와 동작 검증
  - 동일한 입력에 대한 동일한 출력 확인
  - cache_threshold 동작 확인
  - cleanup 동작 확인

**Step 2: 성능 벤치마크**
- [ ] 조회 속도 비교 (text vs SQLite)
  - 1K entries: 순차 조회 시간 측정
  - 10K entries: 랜덤 조회 시간 측정
  - 100K entries: 대용량 조회 시간 측정
- [ ] 삽입 속도 비교
  - 배치 삽입 (1000개) 시간 측정
  - 개별 삽입 vs 트랜잭션 배치 비교
- [ ] 메모리 사용량 비교
  - RSS (Resident Set Size) 측정
  - Text: 모든 항목을 메모리에 로드
  - SQLite: 페이지 캐시만 사용
- [ ] 대용량 캐시 테스트
  - 1M entries 로드 시간
  - 1M entries 조회 성능

**Step 3: 동시성 테스트**
- [ ] 멀티스레드 읽기/쓰기 경합
  ```bash
  # 동시 번역 요청 (10개 스레드)
  for i in {1..10}; do
    curl -X POST http://localhost:8890/translate ... &
  done
  ```
- [ ] 데드락 방지 검증
  - rwlock 정상 동작 확인
  - SQLite FULLMUTEX 모드 확인
- [ ] 스트레스 테스트
  - Apache Bench: `ab -n 1000 -c 10 http://localhost:8890/translate`

**Step 4: Makefile 최종 업데이트**
- [ ] 의존성 추가 확인
  ```makefile
  LIBS = ... -lsqlite3
  ```
- [ ] `make check-deps` 업데이트
  ```makefile
  check-deps:
      @command -v pkg-config >/dev/null || (echo "pkg-config not found" && exit 1)
      @pkg-config --exists libcurl || echo "libcurl not found"
      @pkg-config --exists libmicrohttpd || echo "libmicrohttpd not found"
      @pkg-config --exists libcjson || echo "libcjson not found"
      @pkg-config --exists uuid || echo "uuid not found"
      @pkg-config --exists sqlite3 || echo "sqlite3 not found"
  ```

#### 🚧 1.7 문서화 (향후 작업)

**Step 1: README.md 업데이트**
- [ ] 캐시 백엔드 섹션 추가
  ```markdown
  ## Translation Cache

  ### Backend Types

  Transbasket supports multiple cache backends:

  - **text** (default): JSONL file-based cache
    - Pros: Simple, portable, human-readable
    - Cons: Loads entire cache into memory
    - Best for: Small to medium caches (<10K entries)

  - **sqlite**: SQLite database cache
    - Pros: Fast indexed lookups, low memory usage
    - Cons: Additional dependency
    - Best for: Large caches (>10K entries)

  ### Configuration

  \```
  TRANS_CACHE_TYPE="sqlite"
  TRANS_CACHE_SQLITE_PATH="./trans_cache.db"
  \```

  ### Migration

  \```bash
  ./cache_tool migrate --from text --to sqlite \
      --input trans_dictionary.txt --output trans_cache.db
  \```
  ```

**Step 2: CLAUDE.md 업데이트**
- [ ] 캐시 아키텍처 섹션 추가
  ```markdown
  ## Cache Backend Architecture

  ### Design Pattern

  Plugin-based backend architecture using function pointers:
  - Interface: CacheBackendOps (8 operations)
  - Implementations: text, sqlite, mongodb (future), redis (future)

  ### Adding New Backend

  1. Create header: `include/cache_backend_xxx.h`
  2. Create implementation: `src/cache_backend_xxx.c`
  3. Implement CacheBackendOps interface
  4. Register in trans_cache_init_with_backend()
  ```

**Step 3: 성능 비교 문서**
- [ ] 벤치마크 결과 문서화 (`docs/cache_benchmarks.md`)
  - 조회 속도 그래프
  - 메모리 사용량 그래프
  - 권장 사용 시나리오

---

### 📊 Phase 1 작업 진행 상황 요약

**✅ 완료 (현재 세션)**:
1. ✅ 설정 파일 확장 (1.1) - 100% 완료
2. ✅ SQLite 스키마 설계 (1.2) - 100% 완료
3. ✅ 백엔드 추상화 설계 (1.3) - 100% 완료

**🚧 다음 작업 (다음 세션)**:
4. 🚧 Text 백엔드 분리 (1.3-NEXT) - 구현 가이드 작성됨
5. 🚧 SQLite 백엔드 구현 (1.4) - 상세 설계 완료
6. 🚧 마이그레이션 도구 (1.5) - 설계 완료
7. 🚧 통합 및 테스트 (1.6) - 테스트 계획 작성됨
8. 🚧 문서화 (1.7) - 문서 구조 설계됨

**생성된 파일**:
- ✅ `cache_sqlite_schema.sql` (109 lines)
- ✅ `include/cache_backend_text.h` (25 lines)
- ✅ `transbasket.test.conf` (테스트 설정)
- ✅ `include/trans_cache.h` (백엔드 추상화 적용)
- ✅ `include/config_loader.h` (캐시 설정 추가)
- ✅ `src/config_loader.c` (캐시 설정 파싱 로직)

**수정된 파일**:
- ✅ `transbasket.conf` (캐시 설정 추가, 주석 상세화)

**컴파일 상태**: ✅ 성공 (기존 기능 유지)

**Phase 2: MongoDB/Redis 백엔드 (향후 계획)**
- [ ] `cache_backend_mongodb.c` 구현 (P3)
  - [ ] libmongoc 통합
  - [ ] 연결 풀 관리
  - [ ] 복제본 세트 지원
- [ ] `cache_backend_redis.c` 구현 (P3)
  - [ ] hiredis 통합
  - [ ] Redis Cluster 지원
  - [ ] TTL 기반 자동 만료

---

### 테스트 및 검증
- [ ] 단위 테스트 프레임워크 구축 (Check 또는 CUnit)
  - [ ] 설정 파서 테스트
  - [ ] 프롬프트 템플릿 엔진 테스트
  - [ ] 검증 로직 테스트 (UUID, timestamp, language codes)
  - [ ] emoji 제거 로직 테스트
  - [ ] 캐시 백엔드 테스트
    - [ ] Text 백엔드 (JSONL) 테스트
    - [ ] SQLite 백엔드 테스트
    - [ ] SHA256 해시 충돌 테스트
    - [ ] 캐시 정리 (cleanup) 로직 테스트
- [ ] 통합 테스트
  - [ ] 엔드투엔드 번역 플로우
  - [ ] OpenAI API 모킹 테스트
  - [ ] 에러 시나리오 테스트
  - [ ] 캐시 적중/미스 시나리오
- [ ] 부하 테스트
  - [ ] 동시 요청 처리 능력 측정
  - [ ] 메모리 사용량 모니터링
  - [ ] 스레드 풀 확장성 검증
  - [ ] 캐시 성능 테스트 (대용량 데이터)
- [ ] 장애 주입 테스트
  - [ ] OpenAI API 타임아웃
  - [ ] OpenAI API 5xx 응답
  - [ ] 네트워크 단절
  - [ ] 설정 파일 오류
  - [ ] SQLite DB 파일 손상
  - [ ] 디스크 공간 부족
- [ ] 메모리 안정성 테스트
  - [ ] valgrind를 통한 메모리 누수 검증
  - [ ] 장시간 실행 안정성 테스트
  - [ ] 캐시 메모리 사용량 프로파일링

### 배포 및 운영
- [ ] systemd 유닛 파일 작성
  ```ini
  [Unit]
  Description=Transbasket Translation Service
  After=network.target

  [Service]
  Type=simple
  User=transbasket
  WorkingDirectory=/opt/transbasket
  ExecStart=/opt/transbasket/bin/transbasket -c /etc/transbasket/transbasket.conf -p /etc/transbasket/PROMPT_PREFIX.txt
  Restart=always
  RestartSec=5

  [Install]
  WantedBy=multi-user.target
  ```
- [ ] 배포 스크립트 작성
  - [ ] 의존성 자동 설치
  - [ ] 빌드 및 설치 자동화
  - [ ] 설정 파일 템플릿 배포
- [ ] 운영 가이드 문서 작성
  - [ ] 시작/중지/재시작 방법
  - [ ] 로그 확인 방법 (trace 디렉토리)
  - [ ] 설정 변경 방법 (SIGHUP 재로드)
  - [ ] 트러블슈팅 가이드
  - [ ] 성능 튜닝 가이드 (MAX_WORKERS 설정)
- [ ] 재시작 정책 검증

### 문서화
- [ ] API 문서 업데이트
  - [ ] 에러 응답 예제 추가
  - [ ] 재시도 정책 문서화
- [ ] 개발자 가이드 작성
  - [ ] 모듈별 아키텍처 설명
  - [ ] 코드 스타일 가이드
  - [ ] 기여 가이드
- [ ] 성능 특성 문서화
  - [ ] 처리량 벤치마크 결과
  - [ ] 메모리 사용량 프로파일
  - [ ] 권장 MAX_WORKERS 값 (현재: 2 * CPU_CORES + 1)

### 성능 최적화 및 프로파일링
- [ ] 성능 프로파일링
  - [ ] gprof 또는 perf를 통한 CPU 프로파일링
  - [ ] 병목 지점 식별 및 최적화
  - [ ] 메모리 할당 패턴 분석
- [ ] 메모리 풀 구현 검토 (선택)
- [ ] 코드 리뷰 및 리팩토링
  - [ ] 메모리 관리 패턴 점검
  - [ ] 에러 처리 일관성 검증
  - [ ] 로깅 레벨 최적화

---

## 향후 확장 계획 (우선순위 낮음)

### Structured Outputs (P2)
- [ ] OpenAI Responses API의 JSON 모드 활용
- [ ] 출력 스키마 정의 및 검증 강화
- [ ] JSON Schema 기반 응답 검증

### 모니터링 및 메트릭 (P2)
- [ ] Prometheus 메트릭 엔드포인트 (`/metrics`)
  - [ ] 요청 수 (총, 성공, 실패)
  - [ ] 응답 시간 히스토그램 (p50, p95, p99)
  - [ ] 에러율 (HTTP 상태 코드별)
  - [ ] 활성 연결 수
  - [ ] OpenAI API 재시도 횟수
- [ ] Grafana 대시보드 템플릿
- [ ] 구조화된 로깅 강화 (JSON 로그 형식)

### 보안 강화 (P2)
- [ ] API 키 인증 메커니즘
- [ ] Rate limiting (IP/API 키 기반)
- [ ] HTTPS 지원 (TLS 종료)
- [ ] 요청 크기 제한
- [ ] 입력 sanitization 강화

### 확장성 개선 (P3)
- [ ] 수평 확장 전략 문서화
  - [ ] 로드 밸런서 구성 가이드
  - [ ] 상태 비저장 설계 검증
- [ ] 메시지 큐 통합 검토 (Redis, RabbitMQ)
- [ ] 비동기 작업 처리 옵션
  - [ ] 작업 ID 반환 후 결과 폴링
  - [ ] Webhook 콜백 지원

### 추가 기능 (P3)
- [ ] 번역 품질 개선
  - [ ] 컨텍스트 보존 옵션
  - [ ] 용어집(glossary) 지원
  - [ ] 톤/스타일 설정
- [ ] 다중 모델 지원
  - [ ] 모델 선택 API 파라미터
  - [ ] 모델별 라우팅
- [ ] Docker 컨테이너화
  - [ ] Dockerfile 작성
  - [ ] Docker Compose 설정
  - [ ] 멀티 스테이지 빌드 최적화

---

## 우선순위 가이드

- **P0**: 필수 (프로덕션 배포 전 완료 필요)
- **P1**: 중요 (안정성 및 운영성 향상)
- **P2**: 개선 (편의성 및 관측성)
- **P3**: 향후 고려 (확장 계획)

## 진행 상태 표기

- `[ ]`: 미착수
- `[~]`: 진행 중
- `[x]`: 완료
- `[-]`: 보류/스킵

---

## 참고 문서

- **README.md**: 사용자 가이드 및 API 명세
- **CLAUDE.md**: 개발자 가이드 및 아키텍처 문서
- **transbasket.conf**: 설정 파일 예제
- **PROMPT_PREFIX.txt**: 프롬프트 템플릿
- **cache_sqlite_schema.sql**: SQLite 캐시 스키마 정의 (향후 추가)
