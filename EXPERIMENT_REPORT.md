# EXPERIMENT_REPORT — 인증 상태 관리 실험 기록

각 실험은 **목적 / 가설 / 환경 / 재현 명령어 / 예상 / 실제 / 해석 / 한계** 순서로 적습니다.
실제로 실행한 결과만 적고, 실행하지 않은 것은 "미실행"으로 표시합니다.
원시 결과 파일은 [docs/results/](docs/results/)에 있습니다.

## 공통 환경

| 항목 | 값 |
|---|---|
| 기계 | Apple M1 Pro, 8코어, 16GB |
| OS | macOS 14.8.9 (Darwin 23.6) |
| 컴파일러 | Apple clang 15.0.0, `-std=c11 -O2` (release) |
| Linux 검증 | Docker `microhttp-verify` (Ubuntu 24.04, gcc 13.3, valgrind 3.22) — `tests/linux/Dockerfile` |
| 부하 도구 | hey (`-o csv`, 요청별 원시 시간) |
| 서버 설정 | 기본값: 작업 스레드 8, 큐 64, 유휴 타임아웃 5초, 요청 총 한도 10초, 백엔드 타임아웃 5초 |
| 측정 위치 | 클라이언트·서버·백엔드 모두 같은 기계(루프백) |

---

## STEP 0. 베이스라인 — 인증을 넣기 전의 서버

### 목적
인증 기능을 넣은 뒤 "느려졌는가, 깨졌는가"를 판단할 기준점을 만듭니다.

### 가설
- 처리량·지연은 QUALITY_REPORT 3절과 같은 수준일 것이다.
- 새니타이저(ASan/UBSan/LSan)와 Valgrind는 깨끗할 것이다.
- **TSan은 처음 돌려 보는 것**이고, `stop_requested`(시그널 플래그)를 작업 스레드가 읽는 곳에서 데이터 레이스를 보고할 것이다. (IMPLEMENTATION_REPORT 6절에 "시그널 안전성은 보장되지만 스레드 간 메모리 순서 보장은 아님"으로 이미 적어 둔 한계)

### 재현 명령어

```sh
sh tests/bench/baseline.sh 3 > docs/results/step0_baseline.csv     # 약 5분
python3 tests/bench/summarize.py docs/results/step0_baseline.csv
make test && make asan && make tsan
docker build -t microhttp-verify tests/linux
docker run --rm -v "$PWD":/src:ro microhttp-verify sh /src/tests/linux/verify.sh
```

측정 방법 (`tests/bench/baseline.sh`):
- 시나리오 5개 × 3회, 시나리오별 **중앙값**
- 실행마다 TIME_WAIT 소켓이 1000개 아래로 떨어질 때까지 대기 — 루프백 한 대에서 새 연결이 쌓이면 ephemeral 포트가 고갈되어 지연이 튄다는 것을 QUALITY_REPORT 3절에서 확인했기 때문
- 프록시 시나리오는 백엔드 1개(`tools/backend.py`)

### 실제 결과 — 성능 (2026-11-25, `docs/results/step0_baseline.csv`)

| 시나리오 | 반복 | 요청/초 (중앙값) | 평균 ms | p95 ms | p99 ms | 비정상 응답 합계 |
|---|---:|---:|---:|---:|---:|---:|
| `static_keepalive_c8` — `GET /`, 연결 재사용, 동시 8 | 3 | 37,700 | 0.201 | 0.300 | 0.600 | 0 |
| `static_keepalive_c50` — 연결 재사용, 동시 50 | 3 | 39,471 | 0.991 | 0.300 | 4.700 | 0 |
| `static_newconn_c10` — 요청마다 새 연결, 동시 10 | 3 | 18,248 | 0.537 | 0.900 | 1.200 | 0 |
| `static_newconn_c50` — 요청마다 새 연결, 동시 50 | 3 | 19,096 | 2.473 | 4.400 | 5.300 | 0 |
| `proxy_c10` — `GET /api/x` → 백엔드, 동시 10 | 3 | 5,365 | 1.831 | 2.200 | 2.900 | 0 |

- 서버 fd: 측정 전 7 → 측정 후 7
- 3회 사이 요청/초의 범위: keep-alive c8 37,258~40,883 · c50 37,850~40,420 · 새 연결 c10 18,094~18,657 · c50 18,293~19,443 · 프록시 5,311~6,523

### 실제 결과 — 테스트와 새니타이저

| 검사 | 결과 | 원시 결과 |
|---|---|---|
| `make test` (단위 5 + 기능 28, macOS) | ✅ 28/28 | — |
| `make asan` (ASan + UBSan) | ✅ 28/28, 보고 0 | — |
| `make tsan` (ThreadSanitizer, **처음 실행**) | ❌ 27/28 — `test_graceful_shutdown_finishes_inflight_request` 실패 (서버 종료 코드가 0이 아님) | `docs/results/step0_tsan.txt` |
| Linux `verify.sh` (일반 사용자 `make test`, Valgrind 단위·실서버, malloc 실패 주입) | ✅ `ALL LINUX CHECKS PASSED` — 기능 28/28(사용자 tester), Valgrind 단위 5개 보고 0, Valgrind 실서버 엣지 탐침 28/28·잘못된 접근 0·definite/indirect 누수 0·종료 시 fd = stdio 3 + 로그 1, malloc 실패 주입 500×5 | `docs/results/step0_linux_verify.txt` |

TSan 보고(요청 처리 도중 SIGTERM으로 수동 재현):

```text
WARNING: ThreadSanitizer: data race
  Read of size 4 ... by thread T1:        worker_main server.c:361   (handle_connection이 인라인됨)
  Previous write of size 4 ... by main thread:  on_stop_signal server.c:371
  Location is global 'stop_requested'
```

### 해석
- **성능**: 정적 keep-alive가 새 연결보다 약 2배 빠릅니다(연결 수립·종료 비용). 프록시는 요청마다 백엔드 연결을 새로 맺고 Python 백엔드가 응답하므로 가장 느립니다.
- **c50 keep-alive의 p95(0.3ms)와 p99(4.7ms) 차이**: 연결 50개가 작업 스레드 8개를 기다립니다. 대부분은 스레드를 잡은 연결에서 바로 처리되고, 스레드를 기다린 일부 요청이 꼬리 지연을 만듭니다(스레드 풀 모델의 특성, QUALITY_REPORT 3절).
- **TSan 레이스**: `volatile sig_atomic_t`는 "시그널 핸들러와 같은 스레드" 사이에서만 안전합니다. 다른 스레드가 읽는 것은 C11 메모리 모델상 데이터 레이스이고, 이론적으로 작업 스레드가 셧다운 신호를 늦게 보거나 보지 못할 수 있습니다. 지금까지 테스트에서 문제가 드러나지 않은 이유는 x86/ARM에서 4바이트 정렬 읽기가 실제로는 찢어지지 않고, 작업 스레드는 어차피 큐가 닫히면(`queue.closing`, mutex 보호) 종료되기 때문입니다.
- **수정 계획 (STEP 1 전에)**: 작업 스레드가 읽는 플래그를 C11 `<stdatomic.h>`의 `atomic_int`로 바꾸거나, 이미 mutex로 보호되는 `queue.closing`을 읽게 합니다. 시그널 핸들러에서 `atomic_store`는 lock-free 원자형일 때 안전합니다. STEP 2의 TSan 실험 결과를 이 레이스와 섞이지 않게 읽으려면 먼저 고쳐야 합니다.

### 한계
- hey가 기록하는 시간 해상도는 0.1ms입니다. 1ms 미만 지연의 백분위(예: p95 0.300ms)는 0.1ms 단위로 끊긴 값입니다.
- 루프백 한 대에서 측정했으므로 네트워크 지연이 없고, 부하 도구와 서버가 CPU를 나눠 씁니다.
- 3회 반복이라 분산을 통계적으로 말하기에는 적습니다. 중앙값과 범위만 제시합니다.

### 베이스라인 측정 중 발견해 고친 문제

| 문제 | 발견 경위 | 수정 |
|---|---|---|
| 프록시 "클라이언트 이탈" 오탐 — 응답을 다 받은 클라이언트가 백엔드 EOF보다 먼저 닫으면 `status=-1`로 기록 (1000건 중 116건) | 시범 측정의 서버 로그 | 아무것도 중계하지 않았을 때만 중단으로 처리. 회귀 테스트 추가, 수정 전 FAIL 확인 |
| 시험용 백엔드가 c=10에서 연결을 끊어 프록시 502 (1000건 중 4~5건) | 시범 측정의 비정상 응답 수 | `tools/backend.py` listen 대기열 5 → 128 (서버 코드 변경 없음) |
| p95/p99가 0으로 나옴, 평균 해상도 부족 | 시범 측정 | hey 요약 화면 대신 `-o csv` 원시 시간으로 직접 계산 |
| TSan 보고가 있어도 기능 테스트가 통과할 수 있음 | `make tsan` 추가 중 하니스 검토 | 검사 표식에 `ThreadSanitizer` 추가 |

---

## STEP 0-1. 베이스라인의 TSan 레이스 수정과 회귀 확인

### 목적
STEP 2의 동시성 실험(TSan)을 읽을 때 기존 레이스와 섞이지 않도록, 인증 작업 전에 고칩니다.

### 가설
- `stop_requested`를 lock-free `atomic_int`로 바꾸면 TSan 보고가 사라진다.
- 요청마다 원자적 읽기 1번이 늘 뿐이라 성능 차이는 측정 오차 안에 들 것이다.

### 변경
`src/server.c`: `static volatile sig_atomic_t stop_requested` → `static atomic_int stop_requested`,
읽기는 `atomic_load`, 핸들러의 쓰기는 `atomic_store`, `_Static_assert(ATOMIC_INT_LOCK_FREE == 2)`로 시그널 안전성 조건을 컴파일 시점에 확인.

### 재현 명령어
```sh
make tsan                                    # macOS
docker run --rm -v "$PWD":/src:ro microhttp-verify sh -c \
  'cp -r /src/. /home/tester/w && chown -R tester /home/tester/w && cd /home/tester/w && runuser -u tester -- make tsan'
sh tests/bench/baseline.sh 3 > docs/results/step0_fix_baseline.csv
```

### 실제 결과

| 검사 | 수정 전 (STEP 0) | 수정 후 |
|---|---|---|
| `make tsan` macOS (clang 15) | 27/28, `test_graceful_shutdown_…` FAIL | **28/28** (`docs/results/step0_fix_tsan.txt`) |
| `make tsan` Linux (gcc 13, 사용자 tester) | 미실행 | **28/28** (`docs/results/step0_fix_linux_tsan.txt`) |
| 요청 도중 SIGTERM 수동 재현 (TSan 빌드) | TSan 경고 1, 종료 코드 ≠ 0 | 3회 모두 경고 0, 종료 코드 0 |
| TSan 빌드 서버에 엣지 탐침 30케이스(200병렬 포함) | 미실행 | 30/30, 경고 0, 종료 코드 0 |
| `make test` / `make asan` | 28/28 / 28/28 | 28/28 / 28/28 |
| Linux `verify.sh` (Valgrind 등) | ALL PASSED | ALL PASSED (`docs/results/step0_fix_linux_verify.txt`) |

성능 (같은 스크립트, 3회 중앙값, `docs/results/step0_fix_baseline.csv`):

| 시나리오 | 요청/초 전 → 후 | 3회 범위 전 / 후 | p95 ms 전 → 후 |
|---|---|---|---|
| `static_keepalive_c8` | 37,700 → 42,445 | 37,258~40,883 / 40,298~43,122 | 0.3 → 0.3 |
| `static_keepalive_c50` | 39,471 → 40,298 | 37,850~40,420 / 39,300~42,265 | 0.3 → 0.4 |
| `static_newconn_c10` | 18,248 → 19,894 | 18,094~18,657 / 18,394~19,907 | 0.9 → 0.8 |
| `static_newconn_c50` | 19,096 → 19,011 | 18,293~19,443 / 18,518~19,973 | 4.4 → 4.5 |
| `proxy_c10` | 5,365 → 5,774 | 5,311~6,523 / 5,173~5,814 | 2.2 → 2.2 |

비정상 응답 0 (전후 모두), fd 7 → 7.

### 해석
- **레이스는 사라졌습니다.** 수정 전 TSan이 지목한 읽기(작업 스레드)와 쓰기(시그널 핸들러)가 원자 연산이 되어 보고 대상이 아닙니다.
- **성능 차이는 없다고 판단합니다.** 중앙값은 -0.4% ~ +12.6%로 움직였지만, 모든 시나리오에서 수정 전후의 3회 범위가 겹칩니다. 원자적 읽기 1번이 처리량을 높일 이유가 없으므로, 증가분은 실행 간 변동(같은 기계에서 부하 도구와 서버가 CPU를 나눠 씀)으로 봅니다. 따라서 STEP 1 이후의 비교 기준은 **수정 후 값(`step0_fix_baseline.csv`)**으로 삼습니다.
- `proxy_c10`의 p99가 2.9 → 7.3ms로 커진 것도 3회 중 1회의 꼬리 값이 끌어올린 것으로, p95는 같습니다.

### 한계
- 3회 반복의 변동폭(최대 약 10%)보다 작은 성능 변화는 이 측정으로 구별할 수 없습니다. 이후 STEP에서 "차이 없음"을 주장할 때 이 한계를 함께 적습니다.
