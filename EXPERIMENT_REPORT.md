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

---

## STEP 1. TCP 연결과 Session은 왜 달라야 하나

### 목적
로그인 상태를 "연결"이 아니라 "클라이언트가 들고 다니는 Session ID"에 묶어야 하는 이유를 실제로 확인합니다.

### 가설
- Cookie의 Session ID로 식별하면 로그인한 연결이 닫혀도 새 연결에서 인증이 유지되고, Cookie가 없는 요청은 거부된다.
- fd 번호로 식별하면 커널이 닫힌 fd를 다음 연결에 재사용하므로, **자격 증명이 없는 다른 클라이언트가 로그인한 사용자로 인식**된다.

### 재현 명령어
```sh
make server build/server_fd_demo
python3 tests/experiments/step1_fd_reuse.py          # → docs/results/step1_fd_reuse.md
python3 tests/functional/test_server.py auth         # 인증 기능 테스트 6개
```

### 실제 결과 (`docs/results/step1_fd_reuse.md`) — 요청마다 새 TCP 연결, 서버 로그상 세 연결 모두 fd 5

| 단계 | Session ID 방식 (`./server`) | fd 방식 (`build/server_fd_demo`) |
|---|---|---|
| 1) A 로그인 | 303 `logged in` | 200 `logged in (fd demo)` |
| 2) **B: 자격 증명 없이 /me** | **401** `not logged in` | **200 `user: alice (identified by fd 5)`** |
| 3) A: 새 연결로 /me | 200 `user: alice` (Cookie) | 200 (우연히 같은 fd) |

인증 기능 테스트(`tests/functional/test_server.py`, 6개 모두 PASS):
로그인 후 **새 TCP 연결 3개**에서 같은 Cookie로 200 · Cookie 없음/틀린 비밀번호/없는 사용자/한 글자 바꾼 ID/형식만 맞춘 ID/`user=alice; logged_in=true; role=admin` 위조 Cookie 모두 401 ·
로그아웃 후 옛 Cookie 401 · 재로그인 시 새 ID 발급·이전 ID 401 · **하나의 keep-alive 연결 안에서도 Cookie 없는 요청은 401, 다른 사용자 Cookie면 그 사용자** · 서버 로그에 Session ID 없음.

### 해석
- fd 방식에서 B는 아무것도 보내지 않았는데 alice가 되었습니다. fd는 "지금 이 순간의 연결 번호"일 뿐 사람을 가리키지 않습니다.
- 반대로 연결이 닫힐 때 fd 표를 지우면 이 사고는 막지만, A가 새 연결로 오는 순간 로그인이 풀립니다. 어느 쪽이든 fd는 식별자가 될 수 없습니다.

### 한계
- fd 재사용은 요청을 하나씩 순서대로 보내서 결정적으로 재현했습니다. 동시 연결이 많으면 번호가 섞여 재현이 들쭉날쭉할 수 있습니다.

---

## STEP 2. 만료 · 메모리 해제 · 동시 접근

### 2-1. 만료 (Passive + Active Expiration)

- 설계: 조회할 때 만료면 그 자리에서 삭제하고 갱신하지 않음(Passive, `session_lookup`) + main의 accept 루프가 1초마다 `session_expire()`(Active).
  이 서버는 이벤트 루프가 없는 스레드 풀 구조라, 지침의 "기존 이벤트 루프 활용" 대신 **이미 1초마다 깨어나는 accept 루프의 `poll(1000ms)`**를 타이머로 썼습니다(지침과의 차이, IMPLEMENTATION_REPORT 10절).
- 단위 테스트(`test_session.c`, 시각을 인자로 주입): Idle 59초 사용 → 갱신, 60초 미사용 → 만료 · Absolute는 계속 써도 300초에 만료 · **만료 시점의 조회가 수명을 늘리지 않음** · Active 정리 개수 정확.
- 실서버 (`docs/results/step2_expiration.txt`, `-I 3`): Session 6개(alice 5 + bob 1), bob만 t=2s에 사용 → **요청 없이** 로그에 `expired 2, remaining 4` → `expired 3, remaining 1` → `expired 1, remaining 0`, 만료 후 bob의 `/me` 401.
- 한계: Active 정리는 main 스레드에서 돌기 때문에, 큐가 가득 차 `queue_push`가 기다리는 동안은 정리가 늦어집니다. 정리 자체는 저장소 락을 잡으므로 Session이 매우 많으면 그동안 조회가 기다립니다(연결 리스트 O(n)).

### 2-2. Use-After-Free 재현 → 수정 전후 비교

#### 가설
조회 결과를 노드 안의 **포인터**로 넘기면, 락을 푼 뒤 응답을 만드는 사이에 다른 스레드의 로그아웃(`free`)이 끼어들어 해제된 메모리를 읽는다. 락 안에서 **값을 복사**하면 사라진다.

#### 재현 명령어
```sh
make build/step2_uaf CFLAGS='-std=c11 -O1 -g -fsanitize=address' LDFLAGS=-fsanitize=address
build/step2_uaf unsafe ; build/step2_uaf safe          # → docs/results/step2_uaf_asan.txt
# TSan: 같은 방법으로 -fsanitize=thread → docs/results/step2_uaf_tsan.txt
# Valgrind: tests/linux/verify.sh 3b단계 → docs/results/step2_linux_verify.txt
```
읽는 스레드: 조회 → 락 밖에서 200us "응답 생성" → 사용자 이름 사용. 쓰는 스레드: 같은 Session을 로그아웃 후 재로그인 반복(각 3000회).

#### 실제 결과

| 도구 | unsafe (포인터 반환) | safe (복사본) |
|---|---|---|
| ASan (macOS) | **heap-use-after-free** — READ `reader step2_uaf.c:47`(snprintf), freed by `session_destroy` ← `writer step2_uaf.c:68`, 종료 코드 134 | 보고 없음, 응답 2614 / 조회 시 이미 로그아웃 386, 종료 코드 0 |
| TSan (macOS) | **보고 없음** (응답 2605) | 보고 없음 |
| Valgrind (Linux) | **Invalid read 4건** (`strlen` ← `snprintf`), 종료 코드 99 | Invalid read 0, 종료 코드 0 |

#### 해석
- 해제된 노드를 읽은 것을 ASan이 정확히 잡았고(누가 free했는지까지), 복사본 방식에서는 같은 부하에서도 보고가 없습니다.
- **TSan은 unsafe를 잡지 못했습니다.** 해제된 메모리를 읽는 곳이 libc의 `snprintf` 내부라 TSan의 계측 범위 밖일 가능성이 있습니다(미검증 가설). "TSan이 깨끗하다 = 메모리 안전"이 아니라는 근거로 기록합니다.
- 진행 중 요청과 무효화의 처리 정책: 요청은 **조회 시점의 스냅샷(복사본)**으로 끝까지 처리합니다. 조회 직후 다른 요청이 로그아웃해도 이미 시작한 응답은 그 스냅샷으로 완료되고, 그다음 요청부터 401입니다.

### 2-3. 락 범위 비교 — 넓은 락 vs 좁은 락

#### 가설
(a) 넓은 락: `/me`의 응답 생성과 `send()`까지 저장소 락 안 / (b) 좁은 락: 락 안에서 복사만 하고 해제.
넓은 락은 send 동안 다른 스레드의 조회를 막으므로 락 대기·점유 시간이 길고 처리량이 떨어질 것이다.

#### 환경과 재현 명령어
같은 기계·요청 수(20,000)·동시성(8, 50)·keep-alive, 로그인한 Cookie로 `GET /me`. 두 빌드 모두 `-DSESSION_EXPERIMENTS`(락 통계)로 계측 비용을 같게 맞춤.
```sh
sh tests/bench/step2_lock_scope.sh 3 > docs/results/step2_lock_scope.csv
python3 tests/bench/summarize.py docs/results/step2_lock_scope.csv
```

#### 실제 결과 (3회 중앙값, 괄호는 3회 범위)

| 변형 | c | 요청/초 | p95 ms | p99 ms | 비정상 |
|---|---:|---:|---:|---:|---:|
| 좁은 락 | 8 | **49,975** (45,116~52,645) | 0.3 | 0.5 | 0 |
| 넓은 락 | 8 | **43,002** (39,185~43,169) | 0.3 | 0.6 | 0 |
| 좁은 락 | 50 | 46,339 (41,641~48,228) | 0.3 | 3.1 | 0 |
| 넓은 락 | 50 | 45,652 (43,459~47,037) | 0.3 | 3.4 | 0 |

락 통계 (요청 120,034회 전체, `docs/results/step2_lock_scope.err`):

| 변형 | 평균 점유 | 평균 대기 | 총 대기 |
|---|---:|---:|---:|
| 좁은 락 | 0.213 us | 0.552 us | 66 ms |
| 넓은 락 | 7.261 us (**34배**) | 16.100 us (**29배**) | 1,933 ms |

#### 해석
- **c=8에서는 차이가 있습니다**(약 -14%, 3회 범위가 겹치지 않음). 작업 스레드 8개가 모두 일하고 있어, 한 스레드가 send하는 동안 나머지 7개가 락을 기다리는 시간이 그대로 처리량 손실이 됩니다.
- **c=50에서는 차이가 없다고 판단합니다**(범위가 겹침). 연결 50개가 스레드 8개를 기다리는 구조라 병목이 락보다 앞단(스레드 풀 대기, keep-alive 연결 배정)에 있고, 락 대기는 그 대기 시간에 묻힙니다.
- 루프백에서 응답(약 150바이트)의 `send()`는 커널 버퍼 복사로 수 마이크로초에 끝납니다. **느린 클라이언트**(수신 버퍼가 차서 send가 멈추는 경우)라면 넓은 락은 그 시간 동안 모든 사용자의 인증을 멈추게 하므로 차이가 훨씬 커질 것입니다 — 이 시나리오는 측정하지 않았습니다(미실행).

### 한계
- 락 통계는 실험 빌드에서만 켜지며, 측정 자체(clock_gettime 2회)가 점유 시간에 포함됩니다. 두 변형에 똑같이 들어가므로 비교에는 쓸 수 있지만 절대값은 실제보다 큽니다.

---

## STEP 3. Reverse Proxy를 거치는 인증

### 재현 명령어
```sh
make server && sh tests/experiments/step3_proxy_auth.sh   # → docs/results/step3_proxy_auth.md
```
백엔드 B = 인증을 처리하는 MicroHTTP, 프록시 F = `/login`, `/logout`, `/me`를 B로 넘기는 MicroHTTP(사용자 파일 없음).

### 실제 결과 (요약)

| 확인 | 직접 | 프록시 경유 |
|---|---|---|
| 로그인 / Set-Cookie 속성 / sid 길이 | 303 / `Path=/; HttpOnly; SameSite=Lax` / 64 | 동일 |
| Cache-Control (로그인, /me) | no-store | no-store |
| /me: Cookie 없음 · 자기 sid · 다른 경로에서 받은 sid | 401 · 200 · 200 | 401 · 200 · 200 |
| 로그아웃 → 옛 sid로 /me | Max-Age=0 → 401 | 동일 |
| 응답 Connection | keep-alive | **close** |
| 로그의 Session ID 평문 | 0건 | 0건 |

### 해석
- 프록시는 `Cookie`를 hop-by-hop이 아닌 헤더로 그대로 전달하고, 백엔드 응답(`Set-Cookie`, `Cache-Control`)을 해석 없이 중계하므로 인증 결과가 같습니다.
- `Cache-Control: no-store`는 **백엔드(auth.c)가 붙입니다.** 프록시에는 캐시가 없으므로(지침: 없으면 만들지 않음) 프록시 자체의 캐시 오염 위험은 없고, 이 헤더는 브라우저와 중간 캐시를 위한 것입니다. 백엔드가 이 헤더를 빠뜨리면 프록시가 보완하지 않는다는 점은 한계입니다.
- 실험 중 발견: 처음 스크립트에서 `curl -X POST`(본문 없음)로 로그아웃하자 **411**이 나와 로그아웃이 되지 않았습니다. POST에 Content-Length를 요구하는 기존 파서 규칙 때문이며, 브라우저 폼은 항상 Content-Length를 보내므로 서버가 아니라 스크립트를 `-d ''`로 고쳤습니다.

---

## STEP 7. 다중 서버 문제 확인

### 재현 명령어
```sh
make server && sh tests/experiments/step7_two_servers.sh   # → docs/results/step7_two_servers.md
```

### 실제 결과
- A에서 로그인한 sid로 **A는 200, B는 401**.
- A·B 앞에 Round-Robin 프록시를 두고 로그인 후 `/me` 6번: **200, 401, 200, 401, 200, 401**.
  (프록시의 Round-Robin 순번은 경로 규칙마다 따로 있어서, `/login` 규칙의 첫 요청과 `/me` 규칙의 첫 요청이 둘 다 A로 갔습니다.)

### 해석과 비교 (구현하지 않음)

| 방식 | 원리 | 장점 | 단점 |
|---|---|---|---|
| Sticky Session | 프록시가 같은 사용자를 같은 서버로 보냄(Cookie·IP 기준) | 서버 코드 변경 없음 | 그 서버가 죽으면 로그인 유실, 부하 쏠림 |
| Session Replication | 서버끼리 Session을 복제 | 어느 서버로 가도 됨 | 복제 지연·충돌, 서버 수에 따라 통신량 증가 |
| 공유 저장소(Redis 등) | 모든 서버가 한 저장소를 조회 | 서버는 상태가 없어짐(수평 확장 쉬움) | 저장소가 단일 장애점, 요청마다 네트워크 왕복 |


---

## 전체 회귀 — STEP 0 대비 (인증 STEP 1~3 추가 후)

### 재현 명령어
```sh
make test && make asan && make tsan
sh tests/bench/baseline.sh 3 > docs/results/step2_regression_baseline.csv
docker run --rm -v "$PWD":/src:ro microhttp-verify sh /src/tests/linux/verify.sh   # → docs/results/step2_linux_verify.txt
```

### 실제 결과
| 검사 | STEP 0 | 인증 추가 후 |
|---|---|---|
| `make test` | 단위 5 + 기능 28/28 | 단위 6 + 기능 **34/34** |
| `make asan` | 28/28 | **34/34** |
| `make tsan` | 28/28 (레이스 수정 후) | **34/34** |
| Linux verify.sh | ALL PASSED | **ALL PASSED** (+ Valgrind `test_session`, Valgrind 아래 인증 흐름 200·200·401, UAF unsafe/safe) |


| 시나리오 | STEP 0 (레이스 수정 후) | 인증 추가 후 | 중앙값 변화 | 3회 범위 | 비정상 |
|---|---:|---:|---:|---|---:|
| 정적 keep-alive c8 | 42,445 | 29,326 | -30.9% | 안 겹침 | 0 → 0 |
| 정적 keep-alive c50 | 40,298 | 34,352 | -14.8% | 안 겹침 | 0 → 0 |
| 정적 새 연결 c10 | 19,894 | 18,634 | -6.3% | 겹침 | 0 → 0 |
| 정적 새 연결 c50 | 19,011 | 18,987 | -0.1% | 겹침 | 0 → 0 |
| 프록시 c10 | 5,774 | 5,198 | -10.0% | 겹침 | 0 → 0 |

keep-alive의 큰 하락이 실제 회귀인지 확인하려고, **두 바이너리(STEP 0 `73dc4dd`, 현재)를 같은 시간대에 번갈아 6회씩** 측정했습니다(`docs/results/step2_ab_static.csv`):
STEP 0 중앙값 **35,599** · 현재 **37,884** 요청/초 — 짝지은 6쌍 중 5쌍에서 현재가 같거나 빠름. 같은 세션 안에서 두 바이너리 모두 31k→40k로 함께 움직였으므로(측정 시 load average 약 3),
위 -31%는 두 측정 시점 사이의 환경 변화이고 **회귀는 없다**고 판단합니다. 정적 경로에 추가된 것은 경로 문자열 비교 3번뿐입니다.
기능·메모리 회귀: `make test`/`asan`/`tsan` 모두 단위 6 + 기능 34/34, Linux `verify.sh` ALL PASSED(Valgrind 단위 6개·실서버·인증 흐름 포함). fd는 7 → 9로 시작값이 늘었지만 측정 전후 9 → 9로 일정(libsodium 초기화가 여는 fd로 추정, 누수 아님).

### 측정 중 발견해 고친 문제
- `test_session`의 동시성 테스트에서 만료 스레드가 쉬지 않고 돌아, Valgrind(스레드를 하나씩 실행)에서 다른 스레드가 굶어 11분 넘게 끝나지 않음 → 실제 서버처럼 쉬어 가며(50us) 훑도록 수정. 동시 접근 자체는 유지.
- A/B 측정 CSV의 헤더 이름 오타(요약 스크립트 실패) → 수정, 데이터는 그대로.
