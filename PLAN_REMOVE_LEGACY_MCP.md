# 레거시 MCP 인터페이스 제거 계획

작성 2026-11-06. 작업 위치: worktree `../transbasket-nomcp`, 브랜치 `refactor/remove-legacy-mcp` (main `79a109b` 기준).
요청: "레거시 MCP 코드는 지웁니다." (그 전 결정: "레거시 MCP는 사용할 필요 없습니다. host 에 설치된 최신버전을 기준으로 잘 동작하면 됩니다.")
원칙: 계산·기대값은 python, codex 는 입력일 뿐이며 지적은 전부 코드·실측으로 확인한 뒤 채택, push·서비스 재시작은 요청 시에만.
이 문서의 `파일:줄` 인용은 `79a109b` 기준이다.

## 1. 지우는 것과 남기는 것

"MCP" 라는 말이 두 가지를 가리킨다. **둘째는 지우지 않는다.**

| 대상 | 무엇 | 처리 |
|---|---|---|
| 레거시 인터페이스 | `CODEX_INTERFACE="mcp"` → `codex mcp-server` 를 stdio JSON-RPC 로 구동 (`src/mcp_client.c` 589줄, `include/mcp_client.h` 85줄, `http_client.c:1423-1457` 루프) | **제거** |
| 사용자 config.toml 의 MCP 서버 경고 | 기동 시 `codex mcp list --json` 으로 서버 수·이름을 세어 경고 (`codex_exec.c:381-405`, `http_client.c:701-710`) | **유지** — exec 경로의 보안 경고다 |

현재 배포 설정 (`transbasket_dkfs_codex.conf`, `transbasket_pge4_codex.conf`, `transbasket.local.conf`) 에는
`CODEX_INTERFACE` 줄이 없다 (grep 0건) → 모두 기본값 exec. 추적 템플릿 `transbasket_codex.conf:25` 는 `"exec"`.

## 2. 동작 결정 (권장안)

| # | 항목 | 결정 | 근거 |
|---|---|---|---|
| D1 | `CODEX_INTERFACE="mcp"` (대소문자 무시) + `MODEL_TYPE="codex"` | **기동 실패** `Error: CODEX_INTERFACE "mcp" was removed ...; use "exec"` | 조용히 exec 로 바꾸면 운영자가 명시한 값이 무시된다. LISTEN 때와 같은 원칙 (읽을 수 없는/쓸 수 없는 설정은 기동 중단). `-d` 에서는 launcher 가 exit 1 로 보고 |
| D2 | 같은 값 + `MODEL_TYPE="normal"` | 경고 후 무시, 기동 | normal 모드는 `CODEX_*` 를 쓰지 않는다. codex 모드에서 `OPENAI_*` 를 검증하지 않는 것 (`config_loader.c:276-279`) 과 대칭 |
| D3 | `exec` 외의 다른 잘못된 값 | 현행 유지 (`Warning: Invalid CODEX_INTERFACE ..., using 'exec'`, `config_loader.c:709`) | 이번 범위 밖 |
| D4 | 키 순서·중복 | `MODEL_TYPE` 이 `CODEX_INTERFACE` 뒤에 와도 같은 결과 → 판정은 루프가 끝난 뒤 `validate_config()` 에서. 같은 키가 여러 번이면 **마지막 줄**이 정한다 (현행 덮어쓰기와 같음: `exec`·잘못된 값은 플래그를 false 로 되돌린다) | 줄 단위 판정이면 순서에 따라 달라진다. 한 번 본 `mcp` 를 기억하면 `mcp` 다음 `exec` 를 잘못 거부한다 |
| D5 | `Config` | `CodexInterface` enum·`codex_interface` 필드 삭제, `bool codex_interface_mcp` (요청된 값 기록용) 추가 | 남는 값이 exec 하나뿐 |
| D6 | 기동 로그 | `Codex Interface: exec`, `codex CLI: <ver> (interface=exec)` 형식 유지 | 로그를 grep 하는 쪽 보호 |
| D7 | `codex_cli_has_mcp_server()`, `CODEX_MCP_REMOVED_*`, mcp-server 부재 경고, `SOURCE_HINT_MODE=model` 경고의 `"interface is mcp"` 분기 | 삭제 | 도달 불가 |
| D8 | exec 경로 | 바이트 단위로 변경 없음 (재시도 없음, 504/502 매핑 그대로) | |

## 3. 코드 변경

- 삭제: `src/mcp_client.c`, `include/mcp_client.h`. Makefile 은 `$(wildcard src/*.c)` 라 수정 불필요 (`Makefile:22`) — 테스트 타깃에 명시적 참조 없음 (grep 0건).
- `http_client.c`: `#include "mcp_client.h"` (`:25`), 남는 exec 조건 3곳 (`:682` `use_exec`, `:1339` structured 조건의 interface 항만, `:1400` exec 분기 — 무조건 실행), MCP 루프 (`:1423-1457`), 주석 (`:1287`, `:1396-1399`, `:673-677`), D7 의 경고들.
- `codex_exec.c`/`.h`: D7, `mcp_client_start()` 를 가리키는 주석 (`codex_exec.c:93`), `MCP_RBUF_MAX` 를 가리키는 주석 (`:27`), 헤더 주석 (`codex_exec.h:11-12`).
- `config_loader.c/.h`, `main.c:187-188`: D1-D6.

## 4. 테스트 변경

- 픽스처 `tests/fake_codex_auto.py`, `tests/fake_codex_nul.py`: `mcp-server` 분기 삭제.
- MCP 하위 케이스 삭제: `test_request_coalescing.py` (exec/mcp 루프 4곳, `("mcp","initialize")`·`("mcp","stdin")` 단계), `test_source_auto.py:235`, `test_nul_guard.py:155-170`, `test_log_safety.py:177-194`.
  같은 공유 경로 (병합·마감·공유 실패·종료) 는 exec 하위 케이스가 그대로 덮는다.
- `test_prompt_regression.py`: backend 에서 `mcp` 제외 (새 바이너리는 기동하지 않으므로). 비교 건수 36 → 24 (python 으로 확인).
- 신규 `tests/test_codex_interface.py` + `make test-codex-interface`: D1 (`mcp`, `MCP`, 키 순서 반대), D4 중복 (`mcp`→`exec` 기동, `exec`→`mcp` 실패, `mcp`→잘못된 값 기동), D2, D3, exec 정상 기동, 실패 시 리스너 없음.
  `CODEX_IFACE_OLD_BIN=` 을 주면 이전 바이너리가 `mcp` 로 기동한다는 것도 보인다.
- `test_daemon.py` 실패 기동 목록에 D1 한 줄 추가 (launcher exit 1 + 이유).

## 5. 문서

README, INSTALL (§6, 문제 해결의 `codex MCP initialize failed` 항목, 새 §14 업그레이드 노트 — 문제 해결은 §15 로), CLAUDE.md (모듈 목록, Request Flow, `:140` 예산 문구, `:144` 테스트 문구, codex 절, `:226` 회귀 문구, NUL guard, 크기 표의 `MCP_RBUF_MAX` 행), INSTALL `:225` 예산 문구, README `:584`·`:600` 표 행, `transbasket_codex.conf:99` 주석,
템플릿 2개의 주석, TEST_RESULTS 새 절. `PLAN_MODEL_TYPE_CODEX_MCP.md` 는 머리에 "MCP 경로는 2026-11-06 제거, §7.1 운영 점검표는 유효" 한 줄. 다른 PLAN 문서는 이력이므로 고치지 않는다.

## 6. 검증

1. 잔존 참조 grep: `mcp_client`, `CODEX_INTERFACE_MCP`, `CODEX_INTERFACE_EXEC`, `CodexInterface`, `->codex_interface\b`, `mcp-server`, `MCPClient` 가 src/include/tests 에서 0건 (의도된 문구 제외).
2. 전체 회귀 (기존 20개 타깃 + 신규 + config_limits), `test_prompt_regression.py <79a109b> <new>` 동일.
3. ASan/UBSan 서버 시험, valgrind (exec 경로).
4. 배포 설정 3개 기동 시 경고 0건 (복사본, 네임스페이스 안).
5. 실제 host codex 로 pge4 복사본 `-d -l` exec 번역.

## 7. codex 교차검토 판정 (계획 단계, 코딩 전)

| codex 주장 | 내 검증 방법 | 결과 |
|---|---|---|
| exec 조건 `http_client.c:682`, `:1339`, `:1400` 이 제거 목록에 없다 | 세 줄을 열어 `codex_interface == CODEX_INTERFACE_EXEC` 확인 | ✅ 채택 (§3) |
| 중복 키: 매 줄 덮어쓰고 잘못된 값은 exec 로 되돌림 (`config_loader.c:703-710`) | 해당 줄을 열어 확인 | ✅ 채택 (D4, §4) |
| 문서 누락 INSTALL:225, CLAUDE:140/144/226, README:584/600, `transbasket_codex.conf:99` | 7곳을 열어 MCP 문구 확인 | ✅ 채택 (§5) |
| `validate_config()` 는 성공하는 모든 `load_config()` 경로에서 호출, 재로드 없음, `-d` 는 `main.c:157` 이후 `:176` 에서 로드 | `config_loader.c:859`, `main.c:25,157,176,214`, `daemon.c:138-153` 열람, grep | ✅ (계획 유지) |
| `mcp_client.h` 가 끌어오던 헤더는 다른 경로로 이미 포함 | `mcp_client.h:4-8`, `http_client.c:6-30`, `http_client.h:4-9` 열람 | ✅ |
| MCP 헬퍼는 파일 내부용 | `mcp_client.c:34`, `:257` 이 `static`, 공개 심볼 grep 은 http_client.c·codex_exec.c(주석) 뿐 | ✅ |
| 회귀 36 → 24 | python `3*2*2*3`, `2*2*2*3` | ✅ |

### 구현 diff 검토 (커밋 전)

| codex 주장 | 내 검증 방법 | 결과 |
|---|---|---|
| 잘못된 값 행 (`mcp`→`bogus`, `bogus`) 이 codex 로 떴는지 확인하지 않음 | `tests/test_codex_interface.py` 행 정의와 판정식 열람 | ✅ 채택: 기동 행은 모두 `codex translator initialized`+`(interface=exec)` 또는 `OpenAI translator initialized` 요구 |
| "리스너 없음" 을 실제로 확인하지 않음 | `start()` 열람: 프로세스 종료만 봄 | ✅ 채택: 실패 후 포트 연결 거부 확인 |
| exec 경로 상태 코드·retryable·result_len·call_err 해제 불변 | `git diff HEAD -- src/http_client.c` 직접 대조 (exec 블록은 `if` 만 제거) | ✅ (내 대조와 같음) |
| 잔존 참조 없음 | `grep -rn` (§6 1번 목록) | ✅ |
