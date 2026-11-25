# CODE_FLOW — 요청 하나가 서버를 지나가는 길

이 문서는 실제 코드 순서를 그대로 따라갑니다. 줄 번호는 `docs/kyle-learning-guide` 브랜치 기준입니다.

```text
src/server.c       main, 리스너, 스레드 풀, 연결 처리, 요청 분기, 정적 파일 송신
src/http.c         바이트 → HttpRequest (파서), 응답 헤더 문자열 생성
src/static_file.c  경로 → 파일 (403/404), 확장자 → MIME
src/proxy.c        라우팅 규칙, Round-Robin, 백엔드 전달·중계
src/net.c          send_all, 소켓 타임아웃, 오류 응답, 타임아웃 있는 connect
```

---

## 1. 서버 실행부터 종료까지

```text
./server -p 8080 -r ./www -t 8 -R /api/=127.0.0.1:9001,127.0.0.1:9002

main()                                             server.c:496
 ├─ parse_arguments()                              server.c:456
 │    getopt로 -p -r -t -R -b -v 읽기
 │    -R 마다 proxy_add_route()                    proxy.c:49   → proxy_table.routes[]
 │    realpath(root) → config.root (malloc)         server.c:487
 ├─ install_signal_handlers()                      server.c:375
 │    SIGINT, SIGTERM → on_stop_signal (stop_requested = 1)
 │    SIGPIPE → SIG_IGN
 ├─ create_listener()                              server.c:100
 │    socket(AF_INET, SOCK_STREAM)                 server.c:102
 │    setsockopt(SO_REUSEADDR)                     server.c:110
 │    bind(127.0.0.1:8080)                         server.c:120
 │    listen(BACKLOG=128)                          server.c:128
 ├─ start_workers()                                server.c:404
 │    SIGINT/SIGTERM 막기 → pthread_create × 8 (worker_main) → 마스크 복구
 │
 ├─ while (!stop_requested)                        server.c:529~
 │    poll(listener, 1초)                          server.c:536
 │    accept()                                     server.c:545  → client_fd
 │    queue_push(client_fd)                        server.c:553  (가득 차면 not_full에서 대기)
 │
 └─ 종료 (Ctrl+C 또는 kill)
      close_socket(listener)                       server.c:557  새 연결 중단
      stop_workers()                               server.c:424  closing=1, broadcast, pthread_join
      free(config.root)
      return 0  ("shutdown complete")
```

작업 스레드 8개는 처음부터 끝까지 이 루프를 돕니다.

```text
worker_main()                                      server.c:355
 └─ for (;;)
      client_fd = queue_pop()                      server.c:337  비었으면 not_empty에서 잠듦
      if (client_fd == -1) break                   셧다운 + 큐 비었음 → 스레드 종료
      handle_connection(client_fd)                 server.c:224
      close_socket(client_fd)                      연결을 닫는 곳은 여기 한 곳
```

---

## 2. 클라이언트 연결부터 HTTP 응답까지

### 2.1 정적 파일 요청: `GET /css/style.css HTTP/1.1`

```text
[커널] 3-way handshake 완료 → listen 대기열에 연결 추가
[main] poll이 깨어남 → accept → client_fd=6 → queue_push(6) → pthread_cond_signal(not_empty)
[작업 스레드] queue_pop() = 6

handle_connection(6)                               server.c:224
 ├─ set_socket_timeout(6, 5초)                     net.c:43     SO_RCVTIMEO / SO_SNDTIMEO
 ├─ buffer = malloc(16384 + 65536)                 server.c:230
 └─ for (;;)
     ├─ http_parse_request(buffer, used, ...)      http.c:133
     │    used=0 → HTTP_NEED_MORE
     ├─ recv(6, buffer+used, ...)                  server.c:286  → 53바이트 도착, used=53
     ├─ http_parse_request(...)                    → HTTP_COMPLETE, consumed=53
     │    request = { method "GET", target "/css/style.css", version "HTTP/1.1",
     │                host "localhost:8080", keep_alive 1, body_length 0 }
     ├─ keep_alive = request.keep_alive && 처리수 < 100 && !stop_requested   server.c:262
     ├─ handle_request(6, &request, &keep_alive)   server.c:186
     │    proxy_find_route(&proxy_table, "/css/style.css") → NULL (규칙 없음)
     │    method가 GET → serve_static_file()        server.c:142
     │      static_file_open(root, target, ...)    static_file.c:107
     │        decode_target_path → "/css/style.css"
     │        has_parent_segment → 없음
     │        stat → 일반 파일
     │        realpath → 루트 안인지 확인
     │        open → file_fd, fstat → size=386
     │        mime_type_for_path → "text/css; charset=utf-8"
     │        return 200
     │      http_response_header(...)               http.c:267
     │        "HTTP/1.1 200 OK\r\n...Content-Length: 386\r\nConnection: keep-alive\r\n\r\n"
     │      send_all(6, header)                     net.c:26
     │      while: read(file_fd, 16KiB) → send_all(6, chunk)
     │      close(file_fd)
     │      return 200
     ├─ 로그: [request] fd=6 "GET /css/style.css HTTP/1.1" status=200 keep_alive=1
     ├─ memmove(buffer, buffer+53, used-53)         server.c:273  다음 요청을 앞으로
     └─ 다시 parse → NEED_MORE → recv에서 다음 요청을 기다림
          5초 동안 아무것도 안 오면 EAGAIN → 조용히 break
 free(buffer)                                      server.c:310
[작업 스레드] close_socket(6)
```

### 2.2 프록시 요청: `POST /api/echo` (본문 5바이트)

```text
handle_request()                                   server.c:186
 ├─ proxy_find_route(&proxy_table, "/api/echo")    proxy.c:75   → "/api/" 규칙
 ├─ *keep_alive = 0                                server.c:195  응답 끝을 EOF로 알리므로 연결 닫음
 └─ proxy_forward(client_fd, &request, &proxy_table, route)   proxy.c:204
      ├─ proxy_next_backend()                      proxy.c:92   mutex 안에서 순번 0 → 다음은 1
      ├─ connect_with_timeout("127.0.0.1","9001",2000)  net.c:73
      │    getaddrinfo → socket → O_NONBLOCK → connect=EINPROGRESS
      │    poll(POLLOUT, 2초) → getsockopt(SO_ERROR)=0 → 블로킹으로 복구
      │    (실패하면 다음 백엔드 시도, 모두 실패 → 502)
      ├─ proxy_build_request()                     proxy.c:117
      │    "POST /api/echo HTTP/1.1\r\n" + 원래 헤더(hop-by-hop 제외) + "Connection: close\r\n\r\n"
      ├─ set_socket_timeout(backend, 5초)
      ├─ send_all(backend, header) ; send_all(backend, body 5바이트)
      └─ for (;;) wait_backend_or_client_gone() → recv(backend, 16KiB)
           (기다리는 동안 poll로 클라이언트도 감시: 클라이언트가 EOF/RST면 즉시 중단, status=-1)
           첫 조각: 상태 코드 읽기(response_status) → send_all(client)
           이후 조각: 그대로 send_all(client)
           recv == 0 (백엔드가 닫음) → 끝
           첫 조각 전에 EAGAIN(5초) → 504, 그냥 닫힘 → 502
         close_socket(backend)
handle_connection: keep_alive=0 → break → free(buffer) → 작업 스레드가 close
```

### 2.3 오류 경로 요약

| 어디서 | 상황 | 결과 |
|---|---|---|
| `http_parse_request` | 문법 오류, 크기 초과, 버전 | `send_error_response(status, keep_alive=0)` 후 연결 종료 |
| `handle_connection` recv | 요청 도중 5초 무응답 | 408 후 종료 |
| `handle_connection` | 요청 첫 바이트 후 10초 안에 완성되지 않음(Slowloris) | 408 후 종료 |
| `handle_connection` recv | 요청 도중 EOF | 400 후 종료 |
| `handle_connection` recv | keep-alive 대기 중 무응답·EOF | 조용히 종료 |
| `handle_request` | 정적 경로에 GET/HEAD 외 | 405 + `Allow: GET, HEAD` (연결 유지 가능) |
| `static_file_open` | 없음 / 탈출·권한 / 기타 | 404 / 403 / 500 (연결 유지 가능) |
| `serve_static_file` | 헤더 송신 후 read 실패 | 상태를 바꿀 수 없으므로 연결 종료 |
| `send_all` | 클라이언트가 끊음(EPIPE, SIGPIPE 무시 덕분) | -1 → 연결 종료 |
| `proxy_forward` | 백엔드 연결 불가 / 응답 없음 | 502 / 504 |
| `wait_backend_or_client_gone` | 백엔드를 기다리는 중 클라이언트가 떠남 | 백엔드 요청을 버리고 즉시 종료 (스레드 반환) |

---

## 3. 함수별 역할

| 파일 | 함수 | 한 줄 역할 |
|---|---|---|
| server.c | `main` | 설정 → 시그널 → 리스너 → 스레드 → accept 루프 → 정리 |
| server.c | `parse_arguments` | CLI 인자를 `config`, `proxy_table`에 채움 |
| server.c | `create_listener` | socket → SO_REUSEADDR → bind → listen |
| server.c | `install_signal_handlers` | SIGINT/SIGTERM 플래그, SIGPIPE 무시 |
| server.c | `start_workers` / `stop_workers` | 스레드 시작(시그널 차단) / 큐 닫고 join |
| server.c | `queue_push` / `queue_pop` | 생산자·소비자 작업 큐 (mutex + 조건 변수 2개) |
| server.c | `worker_main` | pop → handle_connection → close 반복 |
| server.c | `handle_connection` | recv 누적 → 파싱 → 요청 처리 → keep-alive 반복 |
| server.c | `handle_request` | 프록시로 보낼지, 정적 파일로 응답할지 결정 |
| server.c | `serve_static_file` | 파일 열기 → 헤더 → 16 KiB씩 read/send |
| http.c | `http_parse_request` | 5단계 파서 (헤더 끝 → 요청 줄 → 헤더 → 규칙 → 본문) |
| http.c | `parse_request_line` | 메서드/경로/버전 검사, 414/505 |
| http.c | `parse_connection` | `close` / `keep-alive` 토큰 |
| http.c | `http_response_header` | 상태 줄 + Content-Type/Length/Connection |
| static_file.c | `static_file_open` | 디코딩 → `..` 차단 → stat → index.html → realpath 확인 → open |
| static_file.c | `mime_type_for_path` | 확장자 → Content-Type |
| proxy.c | `proxy_add_route` / `proxy_find_route` | 규칙 등록 / 가장 긴 prefix 찾기 |
| proxy.c | `proxy_next_backend` | Round-Robin 순번 (mutex) |
| proxy.c | `proxy_build_request` | 백엔드용 요청 헤더 재작성 |
| proxy.c | `proxy_forward` | 연결 → 전달 → 중계, 502/504 |
| net.c | `send_all` | 부분 송신·EINTR 처리 |
| net.c | `set_socket_timeout` | recv/send 대기 한도 |
| net.c | `send_error_response` | "404 Not Found\n" 형식의 짧은 응답 |
| net.c | `connect_with_timeout` | 논블로킹 connect + poll |

---

## 4. 핵심 데이터의 이동

```text
커널 수신 버퍼
   │ recv()
   ▼
buffer (handle_connection의 malloc, 최대 80 KiB)   ← 연결 하나에 하나. free는 함수 끝 한 곳
   │ http_parse_request()  — 복사하지 않고 해석만
   ▼
HttpRequest (스택)
   ├─ method/target/version/host : 고정 크기 배열에 복사 (길이 검사 후)
   ├─ headers, body              : buffer 안을 가리키는 포인터 (빌림)
   └─ keep_alive                 : 연결 유지 여부
   │
   ├─ 정적: target → static_file_open → path(스택) → realpath(malloc→free) → file_fd
   │          file_fd → chunk[16 KiB](스택) → send_all → 커널 송신 버퍼
   │
   └─ 프록시: headers → proxy_build_request → header[16.9 KiB](스택) → send_all(backend)
              body(빌린 포인터) → send_all(backend)
              backend → chunk[16 KiB](스택) → send_all(client)

처리 후: memmove로 남은 바이트(다음 요청)를 buffer 앞으로 당김 → 다시 파싱
```

| 자원 | 만드는 곳 | 해제하는 곳 |
|---|---|---|
| listener fd | `create_listener` | `main` 종료 시 `close_socket(listener)` |
| client fd | `main`의 `accept` | `worker_main`의 `close_socket` |
| 요청 버퍼 | `handle_connection`의 `malloc` | 같은 함수 끝 `free(buffer)` |
| 파일 fd | `static_file_open`의 `open` | `serve_static_file`의 `close(file_fd)` (오류 시 `static_file_open` 안에서 close) |
| realpath 결과 | `static_file_open`의 `realpath(path, NULL)` | 같은 함수에서 `free(real_path)` |
| 백엔드 fd | `connect_with_timeout` | `proxy_forward` 끝 `close_socket(backend_fd)` |
| config.root | `parse_arguments`의 `realpath` | `main` 종료 경로 `free(config.root)` |
| 스레드 | `start_workers`의 `pthread_create` | `stop_workers`의 `pthread_join` |
