# IMPLEMENTATION_REPORT — MicroHTTP 구현 보고서

기준 문서: [REQUIREMENTS.md](REQUIREMENTS.md) (커리큘럼), [CLAUDE.md](CLAUDE.md) (작업 원칙)
검증 환경: macOS 14 (Darwin 23.6, Apple clang 15, MacBookPro18,3 / 8코어), Linux 컨테이너 (Ubuntu 24.04, gcc 13.3)
검증 일자: 2026-11-24

---

> 품질 기준(단위·기능·부하·메모리·엣지 케이스)별 재검증 결과는 **QUALITY_REPORT.md**에 있습니다.

## 1. 기존 코드 분석 (STEP 1)

| 분류 | 항목 | 처리 |
|---|---|---|
| 재사용 | `create_listener`(socket→bind→listen), `send_all`, `close_socket`, SIGPIPE 무시, SO_RCVTIMEO/SO_SNDTIMEO 타임아웃 | `src/server.c`, `src/net.c`로 이동 |
| 재사용 | HTTP 파서 `http_parse_request`(NEED_MORE/COMPLETE/ERROR, 크기 상한, Content-Length 엄격 검사), `http_response_header` | `src/http.c`로 이동 후 확장 |
| 재사용 | `send_all` 단위 테스트(가짜 send) | `tests/unit/test_net.c` |
| 부분 구현 → 보완 | 파서: HTTP/1.1만 허용, keep-alive 판단 없음, 프록시에 넘길 헤더 원문 없음 | HTTP/1.0 허용, `keep_alive` 계산, `headers` 원문 보관 |
| 부분 구현 → 보완 | 응답: `Connection: close` 고정, 채팅용 `Cache-Control: no-store` | `Connection: keep-alive/close` 선택, 채팅 헤더 제거 |
| 미구현 → 구현 | 정적 파일, MIME, 403/500, 동시 처리, SIGINT/SIGTERM 셧다운, Reverse Proxy, 로드 밸런싱, 백엔드 타임아웃 | 아래 3절 |
| 제거 | 축구 라이브 채팅(`football.*`, `chat.*`, `web/match.html`, `tools/embed_page.py`), Ring Buffer 저장소, Polling 측정 로그 | CLAUDE.md 3절 구현 제외 범위 |

---

## 2. 설계 선택과 이유 (STEP 2)

| 선택 | 이유 |
|---|---|
| 소스 5개(`server.c`, `http.c`, `static_file.c`, `proxy.c`, `net.c`) | CLAUDE.md "불필요한 계층 분리 지양". 단위 테스트가 필요한 경계(파서, MIME/경로, 라우팅/프록시)만 파일로 나눔 |
| 동시성: **pthread 스레드 풀** (고정 N개 + 작업 큐) | 커리큘럼 권장. 블로킹 코드를 그대로 쓸 수 있어 흐름이 위→아래로 읽힘. fork는 프로세스마다 메모리 복사, 이벤트 루프는 상태 기계가 필요해 학습 목적상 복잡 |
| 큐가 가득 차면 **main이 기다림**(배압) | 503 즉시 거절은 미수신 요청이 남은 소켓을 닫아 RST로 응답이 유실되는 문제가 있었음(4절 수정 이력) |
| **Reverse Proxy** (경로 prefix 규칙 + Round-Robin) | 필수 항목 중 하나. 동적 요청 처리 요구사항도 "백엔드로 요청 전달"로 함께 충족 |
| 백엔드 연결은 요청 1개당 1개, `Connection: close` | 응답 끝을 EOF로 판단할 수 있어 chunked 해석이 필요 없음 |
| 프록시 응답은 받는 대로 중계(스트리밍) | 응답 전체를 malloc할 필요가 없어 메모리 상한이 고정(16 KiB) |
| 정적 파일은 16 KiB씩 read/send | 파일 크기와 무관하게 메모리 일정, free할 것이 없음 |
| `realpath`로 루트 탈출 재확인 | `..` 차단만으로는 심볼릭 링크 탈출을 막을 수 없음 |
| 실습 백엔드는 Python 표준 라이브러리(`tools/backend.py`) | 서버 본체는 C. 백엔드는 "프록시 뒤 애플리케이션" 역할의 테스트 도구 |

---

## 3. 요구사항별 구현 상태

상태: ✅ 구현·검증 완료, ⚠️ 제한 있음(5절), ❌ 미구현

### 필수 구현 항목

| 요구사항 | 상태 | 파일 / 함수 | 검증 |
|---|---|---|---|
| socket→bind→listen→accept | ✅ | `src/server.c` `create_listener`, `main`(poll+accept) | 모든 기능 테스트 |
| 요청/응답 송수신 | ✅ | `server.c` `handle_connection`(recv 누적), `net.c` `send_all` | `test_net`, 기능 테스트 |
| 요청 라인 파싱 | ✅ | `src/http.c` `parse_request_line` | `test_http` `test_basic_get`, `test_invalid_requests` |
| 헤더 파싱 (Host, Content-Length, Connection) | ✅ | `http.c` `http_parse_request` 3단계, `parse_connection` | `test_http` `test_connection_rules` |
| POST 본문 처리 | ✅ | `http.c` 5단계(Content-Length 만큼), 프록시가 그대로 전달 | `test_post_body_and_split_delivery`, `test_proxy_forwards_post_body_and_headers` |
| GET / POST | ✅ | 정적: GET/HEAD, 프록시 경로: 모든 메서드 | 기능 테스트 |
| 상태 라인 + 표준 헤더 | ✅ | `http.c` `http_response_header` (Content-Type, Content-Length, Connection) | `test_response_header` |
| 본문 송신 | ✅ | `server.c` `serve_static_file`, `proxy.c` `proxy_forward` | 기능·curl 테스트 |
| 루트 디렉터리 정적 파일 | ✅ | `src/static_file.c` `static_file_open`, CLI `-r` | `test_static`, `test_static_files_and_mime` |
| MIME (HTML/CSS/JS/PNG/JPG 등) | ✅ | `static_file.c` `mime_type_for_path` | `test_mime`, 브라우저 확인 |
| 404 / 403 / 500 | ✅ | `static_file_open` + `status_from_errno` | `test_static`(404, 403 ×5), 500은 7절 참고 |
| 동적 요청 처리 | ✅ | 백엔드로 전달: `proxy.c` `proxy_forward` | 프록시 기능 테스트 8개 |
| 동시 요청 처리 (pthread 스레드 풀) | ✅ ⚠️ | `server.c` `queue_push`/`queue_pop`/`worker_main`/`start_workers` | `test_concurrent_*`, `test_pool_exhausted_requests_wait_not_fail`, 부하 테스트 |

### Proxy / Reverse Proxy

| 요구사항 | 상태 | 파일 / 함수 | 검증 |
|---|---|---|---|
| Reverse Proxy (클라이언트 → 프록시 → 백엔드 1..N) | ✅ | `proxy.c` `proxy_forward` | `test_proxy_routes_api_to_backend_and_static_locally` |
| 경로 기반 라우팅 규칙 | ✅ | `proxy_add_route`, `proxy_find_route`(가장 긴 prefix) | `test_proxy` `test_longest_prefix_routing` |
| Round-Robin | ✅ | `proxy_next_backend`(mutex) | `test_round_robin`, `test_proxy_round_robin`, curl |
| 정적은 프록시가 직접 서빙 | ✅ (선택) | `handle_request`: 규칙에 없으면 `serve_static_file` | 기능 테스트 |
| Forward Proxy | ❌ | 요구사항은 "최소 한 가지". Reverse Proxy를 선택 | — |

### 실행 / 빌드 / 환경

| 요구사항 | 상태 | 근거 |
|---|---|---|
| Makefile | ✅ | `make`, `make test`, `make asan` |
| 포트, 루트, 라우팅 규칙을 CLI로 | ✅ | `-p`, `-r`, `-R`, (`-t`, `-b`, `-v`) — `parse_arguments` |
| macOS / Linux 이식성 | ✅ | `_XOPEN_SOURCE=700`(POSIX.1-2008 + XSI), macOS clang·Linux gcc 모두 `-Werror` 빌드 (QUALITY_REPORT 6절에서 수정) |
| 외부 HTTP/이벤트 라이브러리 미사용 | ✅ | libc + POSIX(pthread)만 링크 |

### 중점 포인트

| 항목 | 상태 | 코드 위치 |
|---|---|---|
| SO_REUSEADDR | ✅ | `create_listener` |
| blocking / non-blocking | ✅ | 클라이언트는 블로킹+타임아웃(`set_socket_timeout`), 백엔드 연결은 논블로킹 connect+poll(`connect_with_timeout`) |
| bind 실패 / accept 블로킹 | ✅ | `create_listener`의 `perror("bind")`, `main`의 poll→accept |
| 헤더/본문 경계, Content-Length | ✅ | `http_parse_request` 1단계(`\r\n\r\n`), 5단계 |
| Connection: keep-alive | ✅ ⚠️ | 파서 `keep_alive`, `handle_connection` 반복 + `memmove`(파이프라이닝) |
| malloc/free, 버퍼 오버플로 방지 | ✅ | `handle_connection` 버퍼 1개, `realpath(..., NULL)`+free, `copy_field`/`snprintf` 길이 검사 |
| 끊김·비정상 종료 시 자원 회수 | ✅ | 모든 경로가 `free(buffer)` / `close(file_fd)` / `close_socket` 한 곳을 지남. LSan·leaks·fd 수 확인 |
| SIGINT/SIGTERM 안전한 셧다운 | ✅ | `install_signal_handlers`, `start_workers`(시그널 마스크), `stop_workers` |
| SIGPIPE 무시 | ✅ | `install_signal_handlers` |

---

## 4. 테스트 명령어와 실제 결과

### 4.1 빌드와 단위 테스트

```sh
make clean && make test
```

결과 (macOS, 2026-11-24):

```text
PASS test_http: request line, headers, body, split delivery, keep-alive, errors, limits, response header
PASS test_mime: html, css, js, json, txt, png, jpg, gif, svg, ico, pdf, unknown
PASS test_static: index, query, percent-decode, 404, 403 (.., symlink, permission, dir), 400
PASS test_net: partial send, EINTR, EPIPE, timeout, zero progress, empty input
PASS test_proxy: rule parsing, longest-prefix routing, round-robin, request rewrite (hop-by-hop, Host)
...
25/25 functional tests passed
```

| 요구된 단위 테스트 | 파일 | 결과 |
|---|---|---|
| HTTP Parser | `tests/unit/test_http.c` | PASS |
| MIME 매핑 | `tests/unit/test_mime.c` | PASS |
| Routing | `tests/unit/test_proxy.c` (규칙 파싱, 최장 prefix, Round-Robin) | PASS |
| Proxy 전달 로직 | `tests/unit/test_proxy.c` `test_build_request` (hop-by-hop 제거, Host 보충, 버퍼 부족) | PASS |
| 정적 경로 해석 | `tests/unit/test_static.c` | PASS |
| send_all | `tests/unit/test_net.c` | PASS |

### 4.2 기능 테스트 (실제 TCP, `tests/functional/test_server.py`) — 25/25 PASS

| 분류 | 테스트 |
|---|---|
| 정적·MIME | `test_static_files_and_mime`, `test_static_head_has_no_body`, `test_static_errors_404_403_405` |
| keep-alive | `test_keepalive_reuses_connection`, `test_connection_close_and_http10`, `test_pipelined_requests`, `test_keepalive_idle_timeout_closes_quietly` |
| 비정상 요청 | `test_malformed_requests`(505, 400×4, 411, 501), `test_incomplete_header_then_close`(400), `test_incomplete_header_timeout_408`, `test_slow_request_deadline_408`(Slowloris), `test_large_body_and_long_uri`(413, 414, 431) |
| 연결 종료 | `test_sudden_client_disconnect`(헤더 도중 끊기, RST, 응답 안 읽고 끊기 ×20) |
| 동시 처리 | `test_concurrent_slow_client_does_not_block_others`, `test_concurrent_many_clients`(200연결×10요청, 50병렬), `test_pool_exhausted_requests_wait_not_fail` |
| 셧다운 | `test_graceful_shutdown_finishes_inflight_request`(SIGTERM, SIGINT 각각: 처리 중 요청 200 완료, exit 0, 리스너 닫힘) |
| 프록시 | 라우팅, Round-Robin, POST 본문·헤더 전달, HTTP/1.0 Host 보충, 백엔드 다운 502, 장애 백엔드 건너뛰기, **백엔드 타임아웃 504 (5.00초)**, 응답 전 클라이언트 이탈 |

### 4.3 curl 기능 테스트

```sh
sh tests/functional/curl_check.sh
```

```text
PASS  GET / (index.html)                     -> 200 text/html; charset=utf-8
PASS  GET /css/style.css                     -> 200 text/css; charset=utf-8
PASS  GET /js/app.js                         -> 200 text/javascript; charset=utf-8
PASS  GET /images/logo.png                   -> 200 image/png
PASS  GET /images/photo.jpg                  -> 200 image/jpeg
PASS  HEAD /index.html                       -> 200 text/html; charset=utf-8
PASS  GET /missing (404)                     -> 404 text/plain; charset=utf-8
PASS  GET /../ traversal (403)               -> 403 text/plain; charset=utf-8
PASS  POST static (405)                      -> 405 text/plain; charset=utf-8
PASS  HTTP/1.0 GET                           -> 200 text/html; charset=utf-8
PASS  GET /api/hello (proxy)                 -> 200 application/json; charset=utf-8
PASS  POST /api/echo (proxy body)            -> 200 application/json; charset=utf-8
PASS  GET /down/x (502)                      -> 502 text/plain; charset=utf-8
--- round-robin (backend names):  A B A B
--- keep-alive: "Re-using existing connection with host 127.0.0.1"
```

### 4.4 브라우저 확인

`./server -p 18600` 후 앱 내장 브라우저로 `http://127.0.0.1:18600/` 열기:
CSS 적용(body 배경 `rgb(246,247,249)`), JS 실행(문구 변경), PNG·JPG 로드, 하위 자원 4개 모두 200.

### 4.5 메모리 안전성

| 검사 | 명령 | 결과 |
|---|---|---|
| ASan + UBSan (macOS) | `make asan` | 단위 5개 PASS, 기능 24/24 PASS, 서버 로그에 새니타이저 보고 없음(하니스가 자동 검사) |
| ASan + UBSan + **LeakSanitizer** (Linux) | memdbg 컨테이너에서 `-fsanitize=address,undefined` 빌드, `make unit` | 단위 5개 PASS |
| LSan 서버 실측 (Linux) | 컨테이너 서버 + 호스트 혼합 트래픽(정적·오류·프록시·502·504·끊김, 835요청) → `docker kill --signal TERM` | exit 0, `shutdown complete`, **누수/오류 보고 없음** |
| LSan 동작 대조군 | 같은 플래그로 `malloc(77)` 후 버린 프로그램 | `Direct leak of 77 byte(s)` 검출 → LSan이 실제로 켜져 있음 확인 |
| macOS leaks | `MallocStackLogging=1 leaks --atExit -- ./server ...` + 같은 혼합 트래픽 + SIGTERM | `0 leaks for 0 total leaked bytes` (단, "not debuggable" 경고가 함께 출력되어 보조 근거로만 사용) |
| fd 누수 | 부하 테스트 전후 `lsof -p <pid> | wc -l` | 전 7 → 후 7 (2만~5만 요청 이후 동일) |
| Valgrind | — | **미검증** (macOS·컨테이너에 설치되어 있지 않음. ASan/LSan으로 대체) |

### 4.6 부하 테스트 (`ab`, macOS 루프백, `-O2` 빌드, 작업 스레드 8)

새 연결 / 요청 (`ab -n 3000 -c C http://127.0.0.1:18500/index.html`, 실행 사이 20초 휴식):

| 동시성 c | 요청/초 | 평균 ms | 실패 | 최장 ms |
|---:|---:|---:|---:|---:|
| 1 | 9,963 | 0.100 | 0 | 1 |
| 10 | 18,992 | 0.527 | 0 | 2 |
| 50 | 19,035 | 2.627 | 0 | 7 |
| 100 | 18,973 | 5.271 | 0 | 16 |

keep-alive (`ab -k -n 20000 -c C`):

| 스레드 -t | c | 요청/초 | 실패 | 최장 ms | 해석 |
|---:|---:|---:|---:|---:|---|
| 8 | 1 | 23,386 | 0 | 0 | |
| 8 | 10 | 65,001 | 0 | 13 | |
| 8 | 50 | 1,304 | 24 (Length) | 15,045 | 연결 50 > 스레드 8 → 유휴 keep-alive가 스레드를 5초씩 점유 |
| 8 | 100 | 659 | 48 (Length) | 30,102 | 같은 원인 |
| 8 | 200 | — | ab 중단 | — | `apr_socket_recv: Connection reset by peer` (대기열 + listen backlog 초과) |
| 64 | 50 | 56,596 | 0 | 9 | 스레드 ≥ 연결 → 정상 |
| 64 | 100 | 3,759 | 47 | 5,063 | 다시 스레드 < 연결 |
| 128 | 100 | 62,687 | 0 | 67 | 정상 |

프록시 (`ab -n 2000 -c 20 /api/x`, Python 백엔드 2개): 6,951 요청/초, 실패 0, 서버 로그 `status=200` 2000건.

→ **해석**: 스레드 풀은 "동시에 열린 연결 수 ≤ 스레드 수"일 때 빠르고, 넘으면 keep-alive 유휴 연결이 스레드를 붙잡아 처리량이 급락합니다. 이것이 C10K 문제의 핵심이며 STUDY_GUIDE의 "동시성 모델" 절에서 설명합니다. 커리큘럼 범위(CLAUDE.md "성능 최적화 지양")에 따라 이벤트 루프 전환 같은 최적화는 하지 않았습니다.
"Length" 실패는 서버가 5초 유휴 keep-alive 연결을 닫는 순간 ab가 같은 연결에 다음 요청을 쓴 경우로, keep-alive 연결 종료의 고유한 경쟁 상황입니다(서버 로그에는 오류 응답 없음).

### 4.7 Linux 빌드

```sh
docker run --rm -v "$PWD":/src:ro memdbg:latest sh -c '... make server CC=gcc; make unit ...'
```

결과: Linux 7.0.12-linuxkit / gcc 13.3에서 `-Werror` 일반 빌드 성공, 새니타이저 빌드 단위 테스트 5개 PASS.

> **정정 (2026-11-25)**: c8eedd1을 위 명령으로 다시 빌드하니 `realpath` 미선언 오류로 **실패**했습니다(glibc는 `_POSIX_C_SOURCE`만으로 `realpath`를 선언하지 않음). `_XOPEN_SOURCE=700`으로 수정한 뒤 일반·새니타이저 빌드와 단위 테스트가 모두 통과합니다. 자세한 내용은 QUALITY_REPORT.md 6절.

---

## 5. 실패 → 원인 → 수정 이력

| 증상 | 원인 | 수정 |
|---|---|---|
| macOS에서 `mkdtemp` 미선언 오류 | `_POSIX_C_SOURCE`만 켜면 macOS가 선언을 숨김 | `test_static.c`에 `_DARWIN_C_SOURCE` |
| `/empty/` 요청이 403이 아닌 404 | `/`로 끝나는 경로에 index.html을 먼저 붙여 디렉터리 판별 전에 ENOENT | 디렉터리 확인 후 index.html을 붙이도록 순서 변경 |
| `usleep` 사용 | POSIX.1-2008에서 제거되어 Linux에서 컴파일 오류 가능 | `nanosleep` |
| ASan 빌드에서 503 테스트 `ECONNRESET` | 미수신 요청이 남은 소켓을 응답 직후 close → RST가 503을 덮음 | 큐가 차면 main이 기다리는 배압(`not_full`)으로 변경, 503 제거 |
| ab 결과가 비어 있음 | 비 keep-alive 10만 연결로 클라이언트 ephemeral 포트 고갈(TIME_WAIT) | keep-alive 스윕과 소량 배치+휴식으로 분리 |
| 헤더 원문을 `strstr`로 탐색 | NUL 종료가 보장되지 않는 버퍼 | `memchr`로 범위 제한 |
| Linux에서 `realpath` 미선언 빌드 실패 | glibc는 XSI 매크로가 있어야 선언 | `_XOPEN_SOURCE=700` |
| 16KB 넘는 URI가 431 | 요청 줄이 끝나기 전에 헤더 상한 도달 | 요청 줄에 `\r\n`이 없으면 414 |
| Slowloris로 작업 스레드 무기한 점유 | 타임아웃이 recv 1회 기준 | 요청당 총 한도 10초 → 408 |

---

## 6. 미해결 문제와 한계

| 항목 | 내용 |
|---|---|
| keep-alive + 스레드 풀 | 동시 keep-alive 연결 수가 스레드 수를 넘으면 처리량 급락(4.6). 유휴 타임아웃 5초 |
| 느린 전송(Slowloris) | 요청당 총 한도 10초로 막았지만(408), 연결이 스레드 수만큼 동시에 들어오면 그 10초 동안은 다른 연결이 기다림 |
| 큰 요청의 조기 거절 | 413/414/431 후 남은 입력을 읽지 않고 닫음(lingering close 없음) → 다 보낸 뒤에야 읽는 클라이언트는 응답 대신 RST를 볼 수 있음 |
| 백엔드 타임아웃 | recv 1회 대기 5초 기준. 계속 조금씩 보내는 백엔드는 전체 기한 없음 |
| 프록시 연결 | 프록시 응답 후 클라이언트 연결을 닫음(keep-alive 안 함), 백엔드 연결 재사용 없음 |
| 프록시 헤더 | Connection 헤더 값에 나열된 추가 hop-by-hop 헤더는 제거하지 않음. X-Forwarded-For 없음 |
| HTTP 범위 | chunked 요청 501, Expect 417, 절대 URL 400, HTTP/2 505. 디렉터리 목록·301 리다이렉트 없음 |
| 바인딩 | IPv4만 (`-b`), 기본 127.0.0.1 |
| 셧다운 지연 | keep-alive 대기 중인 작업 스레드는 최대 5초(유휴 타임아웃) 뒤 종료 |
| 시그널 플래그 | `volatile sig_atomic_t`를 작업 스레드도 읽음. 시그널 안전성은 보장되지만 스레드 간 메모리 순서 보장은 아님(학습용 단순화) |
| Forward Proxy | 미구현 (Reverse Proxy 선택) |

## 7. 미검증 항목

> **갱신 (2026-11-25)**: 아래 항목은 모두 이후에 검증했습니다 — Valgrind·Linux 기능 테스트·권한 403·malloc 실패 500은 `tests/linux/verify.sh`, wrk/hey와 ELOOP 500은 QUALITY_REPORT.md 3·5.1절. 아래 표는 당시 기록입니다.

| 항목 | 이유 |
|---|---|
| Valgrind | 설치되어 있지 않음. ASan/UBSan/LSan으로 대체 검증 |
| 500 응답의 실제 발생 | 정상 환경에서 재현하기 어려움(EIO 등). 코드 경로(`status_from_errno` → 500, 헤더 송신 후 read 실패 시 연결 종료)만 존재 |
| Linux에서 Python 기능 테스트 | 컨테이너에 Python이 없어 기능 테스트 스크립트는 macOS에서만 실행. Linux 서버는 호스트 트래픽 + LSan으로 확인 |
| 권한 403 (Linux) | 컨테이너가 root라 권한 검사가 무시되어 해당 assert를 건너뜀(macOS에서는 PASS) |
| wrk / hey | 설치되어 있지 않아 `ab`만 사용 |

---

## 8. 브랜치 구조

| 브랜치 | 내용 |
|---|---|
| `refactor/kyle-webserver-core` | 축구 서비스 제거, 파서 확장, 정적 파일·MIME, keep-alive, CLI |
| `feat/kyle-thread-pool` | 스레드 풀, SIGINT/SIGTERM 셧다운, 배압 수정 |
| `feat/kyle-reverse-proxy` | Reverse Proxy, Round-Robin, 502/504, curl 점검 |
| `docs/kyle-learning-guide` | 이 보고서, CODE_FLOW.md, STUDY_GUIDE.md |
