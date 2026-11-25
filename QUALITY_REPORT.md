# QUALITY_REPORT — 품질 기준별 검증 결과

REQUIREMENTS.md의 "품질" 항목을 기준으로 2026-11-25에 다시 검증한 기록입니다.
모든 수치는 이 날 실제로 실행한 명령의 출력입니다. 실행하지 못한 항목은 마지막 절에 **미검증**으로 적었습니다.

- 대상: `kylekim0868-cpu`(c8eedd1) → 검증 중 발견한 결함 3건을 `fix/kyle-linux-realpath` 브랜치에서 수정 후 재검증
- 환경: macOS 14 (Darwin 23.6, Apple clang), Linux 컨테이너 `memdbg` (Ubuntu 24.04, gcc 13.3)

## 요약

| 품질 기준 | 결과 | 근거 절 |
|---|---|---|
| 단위 테스트 (HTTP 파서, MIME, 라우팅/프록시 전달) | ✅ 5개 모듈 PASS (macOS, Linux, Linux+새니타이저) | 1 |
| 기능 테스트 — curl / 브라우저 | ✅ curl 13/13, 브라우저 하위 자원 4개 200 + 프록시 fetch 200 | 2 |
| 기능 테스트 — 동시 다중 클라이언트 | ✅ 자동 테스트 + 혼합 200병렬 요청 내용 바이트 비교 불일치 0 | 2 |
| 부하 테스트 | ✅ `ab`, `wrk`, `hey` 세 도구로 동시성별 처리량 측정 | 3 |
| 메모리 누수 / 잘못된 접근 | ✅ ASan+UBSan(macOS), ASan+LSan+UBSan(Linux 실서버), **Valgrind memcheck**(Linux 단위 5개 + 실서버), macOS leaks 0 | 4 |
| 엣지 케이스 | ✅ 기능 26/26 (macOS, Linux 일반 사용자) + 엣지 탐침 30/30 (release, ASan, Linux LSan). Valgrind 서버에서는 27/28 — 7절 한계 참고 | 5 |
| 검증 중 발견한 결함 | 🔧 4건 수정: Linux 빌드 실패, 16KB 넘는 URI가 431, Slowloris, 긴 파일 이름이 500 | 6 |
| 500 응답 실제 발생 | ✅ 링크 순환(ELOOP) → 500, malloc 실패 주입 → 500, 전송 중 파일 축소 → 연결 끊김 | 5.1 |

---

## 1. 단위 테스트

```sh
make clean && make unit                                   # macOS clang -Werror
# Linux: memdbg 컨테이너 안에서
make clean && make server unit CC=gcc
make clean && make unit CC=gcc CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,leak,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,leak,undefined"
```

세 환경 모두 같은 출력:

```text
PASS test_http: request line, headers, body, split delivery, keep-alive, errors, limits, response header
PASS test_mime: html, css, js, json, txt, png, jpg, gif, svg, ico, pdf, unknown
PASS test_static: index, query, percent-decode, 404, 403 (.., symlink, permission, dir), 400
PASS test_net: partial send, EINTR, EPIPE, timeout, zero progress, empty input
PASS test_proxy: rule parsing, longest-prefix routing, round-robin, request rewrite (hop-by-hop, Host)
```

| 요구된 모듈 | 테스트 파일 |
|---|---|
| HTTP 파서 | `tests/unit/test_http.c` — 모든 분할 지점에서 같은 결과, 이진 본문, 파이프라이닝, 오류 코드 400/411/413/414/417/431/501/505 |
| MIME 매핑 | `tests/unit/test_mime.c` — 12종 + 대소문자 + 모르는/없는 확장자 |
| 라우팅 / 프록시 전달 | `tests/unit/test_proxy.c` — 규칙 파싱, 최장 prefix, Round-Robin, hop-by-hop 제거, Host 보충 |
| (추가) 정적 경로 해석, send_all | `test_static.c`, `test_net.c` |

## 2. 기능 테스트

### 2.1 자동 기능 테스트 (실제 TCP)

```sh
make test     # 단위 + tests/functional/test_server.py
make asan     # 같은 테스트를 ASan+UBSan 빌드로
```

결과: `26/26 functional tests passed` (macOS release·ASan, Linux gcc 일반 사용자 `tester` 모두). 서버 로그에 새니타이저 보고가 있으면 하니스가 실패로 처리합니다.

### 2.2 curl

```sh
sh tests/functional/curl_check.sh
```

13/13 PASS — 정적 5종 MIME(html, css, js, png, jpg), HEAD, 404, `..` 403, POST 정적 405, HTTP/1.0, 프록시 GET/POST 200, 죽은 백엔드 502.
Round-Robin `A B A B`, keep-alive 연결 재사용(`Re-using existing connection`) 확인.

### 2.3 브라우저

앱 내장 브라우저로 `http://127.0.0.1:18200/` 접속:
CSS 적용(`font-family: system-ui`), PNG·JPG 로드 완료, 하위 자원 `style.css / logo.png / app.js / photo.jpg` 모두 200,
페이지에서 `fetch('/api/hello')` → `200 application/json`, `fetch('/nope')` → 404.

### 2.4 동시 다중 클라이언트

- 자동: `test_concurrent_many_clients`(200연결 × 10요청, 50병렬), `test_concurrent_slow_client_does_not_block_others`, `test_pool_exhausted_requests_wait_not_fail`
- 수동: curl 200개를 동시에 실행(정적 3종 + 프록시 혼합) → 상태 200 × 200, **받은 바이트를 원본 파일과 `cmp`로 비교한 불일치 0**, 전후 fd 7 → 7
- 엣지 탐침: 정상 요청 100 + 쓰레기 요청 100을 200 스레드로 동시에 → 기대 코드(200/400)와 다른 응답 0

## 3. 부하 테스트 (`ab`, macOS 루프백, `-O2`, 작업 스레드 8)

수정 후 빌드 기준. 실행 사이 1~3초 휴식.

| 시나리오 | c | 요청/초 | 평균 ms | p99 ms | 실패 |
|---|---:|---:|---:|---:|---:|
| 새 연결/요청 `GET /` (n=3000) | 1 | 8,065 | 0.12 | 0 | 0 |
| | 10 | 14,594 | 0.69 | 2 | 0 |
| | 50 | 14,080 | 3.55 | 9 | 0 |
| | 100 | 19,058 | 5.25 | 13 | 0 |
| keep-alive `-k` (n=20000) | 1 | 22,253 | 0.05 | 0 | 0 |
| | 8 | 53,749 | 0.15 | 0 | 0 |
| | 16 | 3,732 | 4.29 | 12 | 8 (Length) |
| 프록시 `/api/x` (Python 백엔드 2개, n=1000) | 1 | 3,156 | 0.32 | 1 | 0 |
| | 10 | 2,993~8,154 (단독 3회) | — | 2~14 (단독 7회) | 0 |
| | 50 | 8,025 | 6.23 | 23 | 0 |

해석:
- **keep-alive c=16 급락**: 스레드 8개보다 keep-alive 연결이 많으면 연결이 스레드를 붙잡아 나머지가 기다립니다(스레드 풀 모델의 한계, C10K). IMPLEMENTATION_REPORT 4.6과 같은 현상이며 커리큘럼 범위상 최적화하지 않았습니다.
- **프록시 c=10의 수 초 꼬리 지연(재현 조건 확인)**: 대량의 새 연결 벤치마크 직후에만 p99가 3.5~6.7초로 튀었습니다.
  `netstat`으로 세어 보니 TIME_WAIT 소켓이 **16,063개**로 macOS ephemeral 포트 범위(49152–65535, 16,384개)를 거의 다 쓴 상태였고,
  35초 쉰 뒤(TIME_WAIT 6개) 같은 명령은 p99 3ms였습니다. 서버 로그에 connect 실패·502·504는 없었습니다.
  → 클라이언트·프록시·백엔드를 한 대에서 돌리고, 프록시는 요청마다 TCP 연결을 2개 쓰기 때문에 생기는 **측정 환경의 포트 고갈**입니다.
  실서비스에서는 백엔드 연결 재사용(keep-alive 풀)으로 줄이지만 범위 밖입니다.
- 비교용으로 백엔드(Python `ThreadingHTTPServer`, listen 대기열 5)에 직접 `ab -c 10`을 걸면 첫 요청에서 `Connection reset by peer`로 중단됩니다. 프록시 경유 시에는 실패 0이었습니다.

### 3.1 wrk / hey (같은 서버, 수정 후 빌드)

| 도구 · 시나리오 | c | 요청/초 | p99 | 오류 |
|---|---:|---:|---:|---|
| `wrk -d 10s` keep-alive `GET /` | 1 | 21,421 | 1.38ms | 0 |
| | 8 | 47,876 | 1.87ms | 0 |
| | 16 | 53,376 | 18.6ms | 0 |
| | 64 | 51,821 | 137.7ms | 0 |
| `hey -n 20000` keep-alive | 10 | 33,637 | 0.7ms | 200 × 20000 |
| | 50 | 38,415 | 3.0ms | 200 × 20000 |
| | 100 | 3,615 | 16.8ms | 200 × 20000 |
| `hey -n 3000 -disable-keepalive` | 10 | 14,582 | 2.0ms | 200 × 3000 |
| | 50 | 17,522 | 8.0ms | 200 × 3000 |
| `hey -n 1000` 프록시 `/api/x` | 10 | 7,643 | 6.1ms | 200 × 1000 |
| `wrk -d 3s` 프록시 `/api/x` (TIME_WAIT 식힌 뒤) | 10 | 2,701 | 361ms | 0 (8,165건) |

- wrk는 keep-alive c=16·64에서도 처리량이 유지됐습니다. 연결당 100요청 제한으로 서버가 연결을 닫으면 wrk는 곧바로 다시 연결하기 때문입니다(ab는 이때 Length 실패로 셉니다). hey c=100에서는 ab처럼 급락했습니다 — 스레드 8 < 연결 100.
- **wrk 프록시 10초 실행**은 TIME_WAIT 소켓이 18,627개까지 쌓여 ephemeral 포트 범위(16,384)를 넘었고, 백엔드 connect가 2초 타임아웃 → 두 백엔드 모두 실패 → **502 1건**, wrk timeout 3건이 났습니다. 설계대로의 장애 처리(장애 백엔드 건너뛰기 → 모두 실패 시 502)이며, 측정 환경의 포트 고갈이 원인입니다. 3초 실행에서는 오류 0.
- 서버 fd: 측정 전 7 → 전체 측정 후 7.

## 4. 메모리 안전성

| 검사 | 방법 | 결과 |
|---|---|---|
| ASan + UBSan (macOS) | `make asan` (단위 5 + 기능 25) | 전부 PASS, 보고 0 |
| ASan + UBSan, 실서버 (macOS) | ASan 빌드 서버에 엣지 탐침 30케이스(64MB 다운로드 중 끊기 포함) → SIGTERM | 30/30, exit 0, 로그의 `AddressSanitizer`/`runtime error` 0건, fd 8 → 8 |
| ASan + **LSan** + UBSan, 실서버 (Linux) | 컨테이너 서버에 호스트에서 엣지 탐침 30케이스 + `ab` 정적 5000(c=50) + keep-alive 5000 + 프록시 300 → `docker stop`(SIGTERM) | 30/30, 실패 0, `shutdown complete`, exit 0, **요청 10,571건 처리 후 누수·오류 보고 0**, 종료 직전 fd 4개(stdio + 리스너) |
| macOS leaks | `MallocStackLogging=1 leaks --atExit -- ./server ...` + 엣지 탐침 + ab 3300 → SIGTERM | `0 leaks for 0 total leaked bytes`, fd 10 → 10 |
| **Valgrind memcheck** (Linux) | `tests/linux/verify.sh` 2·3단계: `-O0 -g` 빌드, `--leak-check=full --errors-for-leak-kinds=definite,indirect --error-exitcode=99` | 단위 5개 보고 0 · 실서버에 엣지 탐침 28케이스 후 SIGTERM → exit 0, `shutdown complete`, 잘못된 접근·definite/indirect 누수 0, `--track-fds` 종료 시 열린 fd = stdio 3개 + valgrind 로그 1개(리스너·연결·파일 모두 닫힘) |

## 5. 엣지 케이스

`tests/functional/edge_probe.py`(사용법은 파일 머리말). 각 케이스 뒤에 `GET /`가 200인지 확인해 서버 생존까지 검사합니다.
release · macOS ASan · Linux LSan 세 빌드 모두 **30/30 PASS**.

| 요구 항목 | 케이스 → 기대 결과 |
|---|---|
| 불완전한 헤더 | 헤더 도중 클라이언트 EOF → 400 · 헤더 도중 멈춤 → 408(기능 테스트) · 1바이트씩 0.4초 간격(Slowloris) → **10.1초에 408** |
| 비정상 메서드 | `FOO` → 405 · 소문자 `get` → 405 · `GE(T` → 400 |
| 잘못된 버전 | `HTTP/2.0` → 505 · `HTTP/1.x` → 400 · 버전 없음 → 400 · 1.1인데 Host 없음 → 400 |
| 그 밖의 깨진 요청 | 이진 쓰레기+NUL → 400 · LF만 → 400 · CL+TE 동시(스머글링) → 400 · CL 중복 → 400 · chunked → 501 · `%2e%2e` → 403 · `%zz` → 400 |
| 매우 큰 본문 | CL 10MB(본문 미전송) → 413 · CL 2^70(오버플로) → 413 · 64KB(상한) 프록시 전달 → 200 |
| 매우 긴 URI / 헤더 | URI 10,000B → 414 · **URI 1MB → 414** · 헤더 64KB → 431 |
| 백엔드 무응답 / 타임아웃 | 10초 지연 백엔드 → **5.0초에 504** · 닫힌 포트 → 502 |
| 갑작스러운 연결 종료 | 연결 후 바로 끊기 ×50 · 본문 도중 RST ×50 · 느린 백엔드 대기 중 RST ×20 · 64MB 다운로드 1MB 후 RST · 다운로드 중 읽기 중단(7초) → 모두 서버 생존, fd 원상복구 |

참고(설계상 한계, 수정하지 않음): 413/414/431은 거대한 요청을 끝까지 읽지 않고 바로 응답 후 닫습니다.
curl처럼 보내면서 읽는 클라이언트는 응답을 받지만, **다 보낸 뒤에야 읽는** 클라이언트는 송신 중 EPIPE/RST를 먼저 볼 수 있습니다
(nginx는 "lingering close"로 남은 입력을 잠시 버려 주지만 단순성을 위해 생략).

### 5.1 500 응답과 "헤더 뒤 실패"의 실제 발생

| 상황 | 재현 방법 | 결과 | 검증 위치 |
|---|---|---|---|
| 서버 설정 오류: 심볼릭 링크 순환 | 루트에 `loop_a → loop_b → loop_a` | `stat`이 ELOOP → **500**, keep-alive 유지 | `test_static.c`, `test_server_side_errors_500_and_file_shrinks_midway` |
| 메모리 부족: 연결 버퍼 `malloc(81920)` 실패 | `tests/linux/fail_malloc.c`를 `LD_PRELOAD` (서버 코드 변경 없음) | **500** × 5, 서버 계속 동작 | `tests/linux/verify.sh` 4단계 |
| 200 헤더를 보낸 뒤 `read` 실패 | 32MB 파일 전송 중 `truncate(0)` → `read`가 0 | 상태를 바꿀 수 없으므로 **연결을 끊음**, 클라이언트는 Content-Length보다 덜 받아 실패를 알 수 있음, 서버 계속 동작 | `test_server_side_errors_500_and_file_shrinks_midway` |

## 6. 검증 중 발견해 수정한 결함

| # | 증상 | 원인 | 수정 | 회귀 테스트 |
|---|---|---|---|---|
| 1 | **Linux(glibc)에서 빌드 실패**: `implicit declaration of function 'realpath'` | glibc는 `_POSIX_C_SOURCE=200809L`만으로는 `realpath`를 선언하지 않음(XSI 확장 필요). macOS clang에서는 드러나지 않음 | Makefile `-D_XOPEN_SOURCE=700`, `server.c`도 같은 값 | Linux `make server unit CC=gcc` (-Werror) PASS |
| 2 | 16KB(헤더 상한)를 넘는 URI가 **431**로 응답 | 요청 줄이 끝나기 전에 헤더 상한에 먼저 걸림 | 상한 도달 시 요청 줄에 `\r\n`이 없으면 414 (`http_parse_request`) | `test_http.c` `test_size_limits` — 수정 전 코드에서 FAIL 확인 |
| 3 | **Slowloris**: 1바이트씩 천천히 보내면 작업 스레드를 무기한 점유 (0.4초 간격 → 11초 뒤 200 응답) | `SO_RCVTIMEO` 5초는 recv 1회 기준이라 바이트가 올 때마다 다시 시작 | 요청 하나를 다 받는 총 한도 `REQUEST_TIMEOUT_SECONDS = 10` (`handle_connection`, `CLOCK_MONOTONIC`) → 408 | `test_slow_request_deadline_408` — 수정 전 서버에서 FAIL, 수정 후 10.04초에 408 |

| 4 | 파일 이름 한 조각이 255바이트를 넘는 요청(`/nnn…300자`)이 **500** | `stat`의 ENAMETOOLONG을 "그 밖의 오류"로 분류 → 클라이언트 입력으로 서버 오류(5xx)를 만들 수 있음 | `status_from_errno`: ENAMETOOLONG → 414 | `test_static.c` |

결함 1은 이전 보고서(IMPLEMENTATION_REPORT 4.7)의 "Linux gcc `-Werror` 빌드 성공" 기록과 어긋나 그 기록도 정정했습니다.

## 7. 남은 한계와 미검증 항목

이전에 미검증이던 항목(Valgrind, wrk/hey, Linux Python 기능 테스트, 500 응답, Linux 권한 403)은 모두 위에서 실행했습니다.
Linux 검증은 한 번에 다시 실행할 수 있습니다:

```sh
docker build -t microhttp-verify tests/linux
docker run --rm -v "$PWD":/src:ro microhttp-verify sh /src/tests/linux/verify.sh
```

마지막 실행 결과: `make test`(일반 사용자 tester, 26/26) PASS · Valgrind 단위 5개 PASS · Valgrind 실서버 PASS · malloc 주입 500 PASS ·
**엣지 탐침(Valgrind 서버) 27/28 — FAIL 1건**:

| 남은 한계 | 내용 |
|---|---|
| 프록시 대기 중 클라이언트 이탈을 감지하지 못함 | 느린 백엔드(`/api/slow`)를 기다리는 동안 클라이언트가 끊어도 프록시는 백엔드 타임아웃(5초)까지 작업 스레드를 붙잡습니다. Valgrind로 서버가 느려지면 이런 요청 20개가 스레드 8개를 약 15초 점유해, 탐침의 생존 확인(8초)이 실패합니다. macOS·LSan 실행에서는 RST가 파싱보다 먼저 도착해 드러나지 않았습니다. 다만 끊지 않고 느린 요청만 보내도 같은 점유가 생기므로, 근본 원인은 "블로킹 스레드 풀 + 백엔드 타임아웃" 구조입니다. 테스트 조건은 바꾸지 않았습니다. |

| 미검증 | 이유 |
|---|---|
| EIO 같은 실제 디스크 오류로 인한 500 | 정상 장비에서 재현 불가. 같은 코드 경로(`status_from_errno` → 500)를 ELOOP로 확인 |
| 여러 대에 걸친 부하 테스트 | 모든 측정이 루프백 한 대. 실제 네트워크에서는 수치가 다를 수 있음 |
