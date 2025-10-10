# Transbasket 개발 TODO

## Phase 1: Python POC 개발

### 1.1 API 계약 및 스키마 설계 ✓
- [x] API 명세서 작성 (README.md 완료)
- [x] 요청/응답 JSON 스키마 정의
  - timestamp: RFC 3339 형식
  - uuid: RFC 4122 UUID v4
  - from/to: ISO 639-2 3자리 언어 코드
- [ ] 샘플 페이로드 작성 및 검증
- [ ] 에러 응답 모델 설계
  - 4xx/5xx HTTP 상태 코드 매핑
  - 에러 JSON 스키마 정의

### 1.2 설정 파일 처리
- [ ] transbasket.conf 파서 구현 (configparser 사용)
- [ ] 설정 값 검증 로직 구현
  - OPENAI_BASE_URL 유효성 검증
  - PORT 범위 체크
  - 필수 항목 체크
- [ ] 설정 로드 실패 시 에러 핸들링

### 1.3 FastAPI 기반 HTTP 서버 구현
- [ ] FastAPI 프로젝트 구조 설정
  - 의존성 관리 (requirements.txt 또는 pyproject.toml)
  - 프로젝트 디렉토리 구조 생성
- [ ] `/translate` POST 엔드포인트 구현
  - 비동기 핸들러로 I/O 최적화
  - 요청 JSON 검증 (Pydantic 모델)
  - RFC 3339 timestamp 검증
  - UUID v4 검증
  - ISO 639-2 언어 코드 검증
- [ ] HTTP 연결 유지 로직 구현
  - 번역 완료 시까지 연결 유지
  - 적절한 타임아웃 설정
- [ ] (선택) StreamingResponse 구현
  - 진행 상황 점진 전송
  - 청크 크기 최적화

### 1.4 멀티스레드 처리 구조
- [ ] ThreadPoolExecutor 기반 워커 풀 구현
- [ ] 요청 큐잉 시스템 설계
  - 큐 크기 제한 (메모리 보호)
  - 큐 포화 시 503 응답
- [ ] Async + Thread Pool 하이브리드 아키텍처
  - 네트워크 I/O: async 처리
  - 블로킹 작업: ThreadPoolExecutor로 위임
- [ ] 동시 요청 처리 테스트

### 1.5 OpenAI API 통합
- [ ] OpenAI Python SDK 통합
- [ ] 번역 프롬프트 템플릿 엔진
  - sample_send.txt 기반 프롬프트 생성
  - {{PROMPT_PREFIX}}, {{LANGUAGE_BASE}}, {{LANGUAGE_TO}} 치환
- [ ] API 호출 타임아웃 설정
- [ ] 에러 처리
  - 네트워크 오류
  - API 4xx/5xx 오류
  - 타임아웃 오류
- [ ] 재시도 로직 구현
  - 멱등성 보장 (UUID 기반)
  - 지수 백오프 전략
  - 최대 재시도 횟수 설정

### 1.6 로깅 및 관측성
- [ ] 구조화된 로깅 구현 (JSON 로그)
- [ ] 요청 수명주기 로깅
  - 수신 시각
  - 큐잉 시각
  - OpenAI 호출 시작/종료 시각
  - 응답 전송 시각
  - 각 단계별 소요 시간
- [ ] UUID 기반 요청 추적
- [ ] 에러 로깅 (스택 트레이스 포함)
- [ ] 로그 레벨 설정 (DEBUG, INFO, WARNING, ERROR)

### 1.7 에러 핸들링 및 복원력
- [ ] 일관된 에러 응답 형식
  ```json
  {
    "error": "error_code",
    "message": "Human readable message",
    "uuid": "request_uuid",
    "timestamp": "RFC3339"
  }
  ```
- [ ] HTTP 상태 코드 전략
  - 400: 잘못된 요청 (스키마 위반)
  - 422: 검증 실패 (언어 코드 오류 등)
  - 500: 내부 서버 오류
  - 502: OpenAI API 오류
  - 503: 서버 과부하 (큐 포화)
  - 504: OpenAI API 타임아웃
- [ ] 재시도 가능 여부 헤더 추가 (Retry-After)

### 1.8 테스트
- [ ] 단위 테스트
  - 설정 파서 테스트
  - 프롬프트 템플릿 엔진 테스트
  - 검증 로직 테스트
- [ ] 통합 테스트
  - 엔드투엔드 번역 플로우
  - OpenAI API 모킹 테스트
- [ ] 부하 테스트
  - 동시 요청 처리 능력
  - 큐 포화 시나리오
  - 메모리 사용량 모니터링
- [ ] 장애 주입 테스트
  - OpenAI API 타임아웃
  - OpenAI API 5xx 응답
  - 네트워크 단절

### 1.9 배포 및 운영
- [ ] systemd 유닛 파일 작성
  ```ini
  [Unit]
  Description=Transbasket Translation Service
  After=network.target

  [Service]
  Type=simple
  User=transbasket
  WorkingDirectory=/opt/transbasket
  ExecStart=/opt/transbasket/venv/bin/python -m transbasket
  Restart=on-failure
  RestartSec=5s

  [Install]
  WantedBy=multi-user.target
  ```
- [ ] 배포 스크립트 작성
- [ ] 운영 가이드 문서 작성
  - 시작/중지/재시작 방법
  - 로그 확인 방법
  - 설정 변경 방법
  - 트러블슈팅 가이드
- [ ] 헬스체크 엔드포인트 구현 (`/health`)
- [ ] 재시작 정책 검증

### 1.10 POC 완료 및 정리
- [ ] 코드 리뷰 및 리팩토링
- [ ] 문서 업데이트
- [ ] 성능 메트릭 수집 및 분석
- [ ] 기능 동결 및 버전 태깅

---

## Phase 2: C 언어 포팅

### 2.1 아키텍처 설계
- [ ] Python POC와 동일한 API 계약 준수 확인
- [ ] 모듈 경계 설계
  - HTTP 서버 모듈
  - HTTP 클라이언트 모듈 (OpenAI API)
  - JSON 파서 모듈
  - 설정 파서 모듈
  - 스레드 풀 모듈
  - 로깅 모듈
- [ ] 의존성 라이브러리 선정
  - HTTP 서버: libmicrohttpd, libevent, 또는 커스텀
  - HTTP 클라이언트: libcurl
  - JSON: cJSON, jansson, 또는 jsmn
  - UUID: libuuid
  - 스레드: pthreads

### 2.2 핵심 모듈 구현
- [ ] 설정 파일 파서 (`config.c`, `config.h`)
- [ ] HTTP 서버 (`http_server.c`, `http_server.h`)
  - 비동기 I/O 처리
  - 요청 라우팅
  - 연결 관리
- [ ] JSON 처리 (`json_utils.c`, `json_utils.h`)
  - 요청 파싱
  - 응답 생성
  - 스키마 검증
- [ ] 스레드 풀 (`thread_pool.c`, `thread_pool.h`)
  - 워커 스레드 관리
  - 작업 큐 관리
- [ ] OpenAI API 클라이언트 (`translator.c`, `translator.h`)
  - libcurl 기반 HTTP 클라이언트
  - 재시도 로직
  - 타임아웃 처리
- [ ] 로깅 시스템 (`logger.c`, `logger.h`)
  - 구조화된 로깅
  - 로그 레벨 관리

### 2.3 메모리 관리 및 최적화
- [ ] 메모리 누수 방지
  - valgrind를 통한 메모리 검증
  - 모든 할당/해제 추적
- [ ] 메모리 풀 구현 (선택)
- [ ] 버퍼 오버플로우 방지
- [ ] 성능 프로파일링
  - gprof 또는 perf를 통한 분석
  - 병목 지점 식별 및 최적화

### 2.4 에러 처리
- [ ] 일관된 에러 코드 체계
- [ ] 에러 전파 메커니즘
- [ ] 복구 전략 구현

### 2.5 테스트
- [ ] 단위 테스트 (Check 또는 CUnit)
- [ ] 통합 테스트
- [ ] Python POC와 동일한 승인 테스트
  - 동일한 입력에 대한 동일한 출력
  - 성능 회귀 검증
- [ ] 메모리 안정성 테스트
- [ ] 부하 테스트

### 2.6 배포 및 문서화
- [ ] Makefile 또는 CMakeLists.txt 작성
- [ ] 설치 스크립트
- [ ] systemd 유닛 파일 (Python POC 것을 수정)
- [ ] 빌드 및 배포 문서
- [ ] 성능 비교 리포트 (Python vs C)

---

## 확장 계획 (향후)

### Structured Outputs
- [ ] OpenAI Responses API의 JSON 모드 활용
- [ ] 출력 스키마 정의 및 검증 강화

### 모니터링 및 메트릭
- [ ] Prometheus 메트릭 엔드포인트 (`/metrics`)
  - 요청 수
  - 응답 시간 (p50, p95, p99)
  - 에러율
  - 큐 크기
  - 스레드 풀 사용률
- [ ] Grafana 대시보드

### 보안
- [ ] API 키 인증
- [ ] Rate limiting
- [ ] HTTPS 지원

### 확장성
- [ ] 수평 확장 전략 (로드 밸런서)
- [ ] 메시지 큐 통합 (Redis, RabbitMQ)
- [ ] 비동기 작업 처리 (작업 ID 반환 후 결과 폴링)

---

## 우선순위 태그

- `P0`: 필수 기능 (POC 동작에 필수)
- `P1`: 중요 기능 (안정성 및 운영에 중요)
- `P2`: 개선 사항 (편의성 향상)
- `P3`: 향후 고려 (확장 계획)

## 진행 상태

- `[ ]`: 미착수
- `[~]`: 진행중
- `[x]`: 완료
- `[-]`: 보류/스킵
