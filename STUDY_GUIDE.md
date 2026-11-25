# STUDY_GUIDE — 웹 서버 핵심 원리 (WWHWC)

각 개념을 **What**(무엇) / **Why**(왜) / **How**(어떻게) / **Without**(없으면) / **Code**(우리 코드 어디)로 정리했습니다.
Code 항목의 함수는 모두 실제로 존재하며, 흐름은 [CODE_FLOW.md](CODE_FLOW.md)에 이어서 나옵니다.
각 절 끝의 **확인 질문**에 코드를 보지 않고 답할 수 있으면 그 개념을 이해한 것입니다.

추천 읽기 순서: 1 → 4 → 6 → 9 → 10 → 12 (나머지는 필요할 때)

---

## 1. TCP 소켓 서버의 표준 흐름 (socket → bind → listen → accept)

- **What**: 운영체제에게 "이 포트로 오는 TCP 연결을 받겠다"고 등록하고, 완성된 연결을 하나씩 꺼내 쓰는 4단계 시스템 콜.
- **Why**: 네트워크 카드와 TCP 프로토콜은 커널이 관리합니다. 프로그램은 소켓(fd)이라는 손잡이로만 커널에 부탁할 수 있습니다.
- **How**:
  1. `socket()`: TCP용 fd를 만듭니다 (아직 주소 없음).
  2. `bind()`: 이 fd에 IP:포트를 붙입니다. 포트를 다른 프로세스가 쓰고 있으면 `EADDRINUSE`로 실패합니다.
  3. `listen()`: 커널이 이 포트로 오는 SYN에 응답해 **3-way handshake(SYN → SYN-ACK → ACK)를 대신 끝내고**, 완성된 연결을 backlog 대기열에 쌓기 시작합니다.
  4. `accept()`: 대기열에서 완성된 연결 하나를 꺼내 **새 fd**로 돌려줍니다. 대기열이 비어 있으면 연결이 올 때까지 블로킹합니다. listener fd는 계속 다음 연결을 받는 용도로 남습니다.
- **Without**: `listen` 없이는 SYN이 와도 커널이 RST로 거절합니다(Connection refused). `accept` 없이는 handshake가 끝난 연결이 대기열에 쌓이기만 하고, 대기열이 차면 새 연결이 거절되거나 지연됩니다.
- **Code**: `src/server.c` `create_listener()` (socket / bind / listen), `main()`의 `poll` → `accept` → `queue_push`.

**확인 질문**
- `accept()`가 돌려준 fd와 listener fd는 무엇이 다른가?
- `listen(fd, 128)`의 128은 "동시에 128개를 처리한다"는 뜻인가? (아니라면 무엇인가?)
- handshake는 누가 하는가? `accept()`를 호출하기 전에도 클라이언트의 `connect()`가 성공할 수 있는 이유는?

---

## 2. SO_REUSEADDR

- **What**: 아직 TIME_WAIT 상태인 이전 연결이 남은 포트에도 `bind`를 허용하는 소켓 옵션.
- **Why**: 서버를 껐다가 바로 켜면 이전 연결이 TIME_WAIT(늦게 도착한 패킷을 처리하려고 수십 초 유지)로 남아 bind가 실패합니다.
- **How**: `bind` 전에 `setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &1)`를 호출하면, 커널이 TIME_WAIT 연결과 주소가 겹쳐도 bind를 허락합니다. 이미 LISTEN 중인 다른 프로세스와의 충돌은 여전히 막습니다.
- **Without**: 재시작할 때마다 `bind: Address already in use`가 나서 30~60초를 기다려야 합니다.
- **Code**: `src/server.c` `create_listener()`의 `setsockopt(... SO_REUSEADDR ...)`.

**확인 질문**
- TIME_WAIT은 서버와 클라이언트 중 누구에게 생기는가? (먼저 close한 쪽)
- 부하 테스트에서 클라이언트(ab)의 포트가 고갈된 것도 TIME_WAIT 때문이었다. 왜 클라이언트 쪽에 쌓였을까?

---

## 3. Blocking vs Non-blocking I/O, 타임아웃

- **What**: 블로킹 소켓은 데이터가 올 때까지 `recv`/`accept`/`connect`에서 멈춥니다. 논블로킹 소켓은 할 일이 없으면 즉시 `EAGAIN`/`EINPROGRESS`로 돌아옵니다.
- **Why**: 블로킹은 코드를 위에서 아래로 쓸 수 있어 읽기 쉽지만, 상대가 아무것도 보내지 않으면 스레드가 영원히 멈춥니다.
- **How**:
  - 클라이언트 쪽은 **블로킹 + 타임아웃**입니다. `SO_RCVTIMEO`/`SO_SNDTIMEO`를 5초로 걸면, 5초 동안 진행이 없을 때 `recv`가 `EAGAIN`으로 돌아옵니다.
  - 이 5초는 recv **한 번**의 대기 한도라 바이트가 올 때마다 다시 시작됩니다. 그래서 요청 하나를 다 받는 **총 한도**(`REQUEST_TIMEOUT_SECONDS` 10초)를 따로 둡니다. 첫 바이트 시각을 `CLOCK_MONOTONIC`으로 기록하고, 아직 NEED_MORE인데 10초가 지났으면 408을 보냅니다.
  - 백엔드 연결은 **논블로킹 connect + poll**입니다. `O_NONBLOCK`으로 바꾸면 `connect`가 즉시 `EINPROGRESS`로 돌아옵니다. 그다음 `poll(POLLOUT, 2초)`로 연결 완료를 기다리고, `SO_ERROR`로 성공 여부를 확인한 뒤 블로킹으로 되돌립니다.
- **Without**: 헤더를 보내다 멈춘 클라이언트 하나가 작업 스레드를 영원히 붙잡습니다. 응답 없는 백엔드 IP에 블로킹 connect를 하면 OS 기본값(수십 초 이상)만큼 멈춥니다.
- **Code**:
  - `src/net.c` `set_socket_timeout()`, `connect_with_timeout()`
  - `src/server.c` `handle_connection()`의 `EAGAIN` → 408, `request_started` + `monotonic_seconds()` → 408(총 한도)
  - `src/proxy.c` `proxy_forward()`의 시간 초과 → 504, `wait_backend_or_client_gone()`: 백엔드를 기다리는 동안 `poll`로 클라이언트도 지켜보다가 떠나면 즉시 중단
- 테스트: `test_incomplete_header_timeout_408`, `test_slow_request_deadline_408`, `test_proxy_backend_timeout_504`

**확인 질문**
- 우리 타임아웃 5초는 "요청 전체를 5초 안에"인가, "recv 한 번이 5초 안에"인가? 그 차이로 남는 공격(Slowloris)은?
- 논블로킹 connect가 성공했는지는 `poll`의 결과만으로 알 수 있는가?

---

## 4. HTTP 메시지의 경계 (요청 줄 / 헤더 / 본문, Content-Length)

- **What**: TCP는 "바이트의 흐름"이라 메시지의 시작과 끝을 알려주지 않습니다. HTTP/1.1은 다음 규칙으로 경계를 정합니다.
  - 헤더는 빈 줄 `\r\n\r\n`에서 끝납니다.
  - 본문은 `Content-Length` 바이트만큼입니다.
- **Why**: `recv` 한 번에 요청 반쪽이 올 수도 있고, 요청 두 개가 붙어서 올 수도 있습니다. 경계를 정확히 알아야 요청을 나누고 다음 요청을 이어서 읽을 수 있습니다.
- **How**: `http_parse_request`는 받은 바이트 전체를 매번 처음부터 5단계로 검사합니다.
  1. `\r\n\r\n`을 찾습니다. 없으면 `NEED_MORE`이고, 16 KiB를 넘으면 431입니다.
  2. 요청 줄 `METHOD SP TARGET SP VERSION`을 해석합니다.
  3. 헤더를 줄마다 `이름: 값`으로 해석합니다. Host, Content-Length(숫자만, 중복 거절), Connection을 읽습니다.
  4. 규칙을 검사합니다: HTTP/1.1은 Host 필수, Content-Length와 Transfer-Encoding이 함께 오면 400, POST는 길이가 필수(411).
  5. 받은 본문이 `Content-Length`보다 적으면 `NEED_MORE`이고, 다 왔으면 `COMPLETE`와 함께 `consumed`(이 요청의 바이트 수)를 돌려줍니다.
- **Without**: `recv` 한 번을 요청 하나로 보면, 쪼개진 요청은 깨지고 붙어 온 요청은 사라집니다. Content-Length를 느슨하게 해석하면(중복 허용 등) 프록시와 백엔드가 경계를 다르게 보는 **request smuggling** 공격의 빌미가 됩니다.
- **Code**:
  - `src/http.c` `http_parse_request()`, `parse_request_line()`
  - `src/server.c` `handle_connection()`의 recv 누적 루프
  - 테스트: `test_http.c` `test_post_body_and_split_delivery`(모든 분할 위치), `test_binary_body_and_pipelining`

**확인 질문**
- 파서가 `NEED_MORE`를 돌려주면 서버는 무엇을 하는가? 버퍼는 비우는가, 이어 붙이는가?
- 본문에 `\0`이 있어도 잘리지 않는 이유는? (`strlen`을 쓰지 않음)
- `Content-Length: 99999999999999999999`가 오면 정수 오버플로 없이 413을 내는 방법은?

---

## 5. HTTP 응답 생성

- **What**: 응답은 상태 줄 + 헤더 + 빈 줄 + 본문으로 이루어집니다. 예: `HTTP/1.1 200 OK`, `Content-Type`, `Content-Length`, `Connection`.
- **Why**:
  - `Content-Type`이 있어야 브라우저가 내용을 어떻게 해석할지 압니다.
  - `Content-Length`가 있어야 본문이 어디서 끝나는지 압니다.
  - `Connection`이 있어야 연결을 유지할지 압니다.
- **How**:
  - `snprintf`로 헤더를 만들고, 잘렸는지(반환값 ≥ capacity) 검사합니다.
  - 값에 `\r\n`이 섞이면 거절합니다(헤더 주입 방지).
  - 헤더를 보낸 다음 본문을 따로 보냅니다.
  - `send`는 일부만 보낼 수 있으므로 `send_all`이 다 보낼 때까지 반복합니다.
- **Without**: Content-Length 없이 keep-alive를 하면 클라이언트는 본문이 어디서 끝나는지 알 수 없습니다. 부분 송신을 무시하면 응답이 잘립니다.
- **Code**:
  - `src/http.c` `http_response_header()`, `http_reason()`
  - `src/net.c` `send_all()`, `send_error_response()`
  - 테스트: `test_net.c`(가짜 send로 부분 송신과 EINTR 재현)

**확인 질문**
- `send()`가 100바이트 중 30바이트만 보냈다고 반환하면 왜 오류가 아닌가?
- 헤더를 이미 보낸 뒤 파일 읽기가 실패하면 500으로 바꿀 수 있는가? 우리 코드는 어떻게 하는가?

---

## 6. Connection: keep-alive

- **What**: 응답 뒤에도 TCP 연결을 닫지 않고 같은 연결로 다음 요청을 받는 방식입니다.
  - HTTP/1.1은 기본으로 유지하고, `Connection: close`가 있으면 닫습니다.
  - HTTP/1.0은 기본으로 닫고, `Connection: keep-alive`가 있으면 유지합니다.
- **Why**: 요청마다 handshake(1 RTT)를 반복하지 않아도 되고, TIME_WAIT 소켓과 포트 소모도 줄어듭니다.
- **How**:
  - 파서가 버전과 Connection 토큰으로 `request.keep_alive`를 계산합니다.
  - 서버가 실제로 유지할지는 응답 헤더 `Connection: keep-alive/close`로 알립니다.
  - 유지하는 경우 `consumed` 바이트를 `memmove`로 버퍼에서 지우고, 이미 함께 도착한 다음 요청(파이프라이닝)이 있으면 바로 이어서 파싱합니다.
  - 연결 하나가 서버를 독점하지 않도록 연결당 100요청, 유휴 5초로 제한합니다.
- **Without**: 이미지가 30개인 페이지는 연결을 30번 맺습니다. 부하 테스트에서는 연결마다 쓰는 포트가 금방 고갈됩니다(4.6절 참고).
- **Code**:
  - `src/http.c` 4단계의 `request->keep_alive`
  - `src/server.c` `handle_connection()`의 `keep_alive` 계산과 `memmove`
  - 테스트: `test_keepalive_reuses_connection`, `test_pipelined_requests`, `test_keepalive_idle_timeout_closes_quietly`
- **우리 서버의 대가**: 스레드 풀에서는 유휴 keep-alive 연결도 스레드 하나를 붙잡습니다. 측정 결과 연결 50개 대 스레드 8개에서 처리량이 65,001에서 1,304 요청/초로 떨어졌습니다([IMPLEMENTATION_REPORT 4.6](IMPLEMENTATION_REPORT.md)).

**확인 질문**
- 프록시 요청 뒤에는 왜 keep-alive를 하지 않는가? (백엔드 응답의 끝을 EOF로 판단하기 때문)
- 파이프라이닝으로 요청 두 개가 한 번에 왔을 때 두 번째 요청은 어디에 있다가 언제 처리되는가?

---

## 7. 정적 파일 서빙과 MIME 타입

- **What**: 요청 경로를 루트 디렉터리 아래의 파일로 바꿔서 그 내용을 보내는 것입니다. 이때 Content-Type은 확장자로 정합니다.
- **Why**: HTML, CSS, JS, 이미지처럼 바뀌지 않는 파일은 프로그램 로직 없이 바로 보낼 수 있습니다. 브라우저는 확장자가 아니라 **Content-Type 헤더**를 보고 해석합니다.
- **How**: `static_file_open`이 다음 순서로 처리합니다.
  1. `?` 뒤를 버리고 `%XX`를 디코딩합니다.
  2. `..` 조각이 있으면 403입니다.
  3. `stat`으로 확인합니다. 디렉터리면 `index.html`을 찾고, 없으면 404입니다.
  4. `realpath`로 실제 위치가 루트 안인지 확인합니다.
  5. `open`/`fstat`으로 크기를 구하고, 확장자로 MIME 타입을 정합니다.

  그다음 `serve_static_file`이 16 KiB씩 `read`해서 `send_all`로 보냅니다.
- **Without**: CSS를 `text/plain`으로 보내면 스타일이 적용되지 않고 JS는 실행되지 않습니다. 브라우저로 직접 확인했습니다.
- **Code**:
  - `src/static_file.c` `static_file_open()`, `mime_type_for_path()`, `status_from_errno()`(ENOENT→404, EACCES→403, 기타→500)
  - `src/server.c` `serve_static_file()`
  - 테스트: `test_mime.c`, `test_static.c`, `test_static_files_and_mime`

**확인 질문**
- 파일 전체를 `malloc`해서 보내지 않고 조각으로 보내면 무엇이 좋은가?
- `/docs`와 `/docs/`는 각각 어떤 파일을 보내는가? 디렉터리에 index.html이 없으면?

---

## 8. 경로 탈출 방지 (403)

- **What**: `GET /../../etc/passwd`처럼 루트 밖의 파일을 요청하는 공격(path traversal)을 막는 것입니다.
- **Why**: 정적 서버가 디스크 전체를 노출하면 안 됩니다.
- **How**: 두 겹으로 막습니다.
  1. 디코딩한 뒤 `..` 조각을 거절합니다. `%2e%2e`도 디코딩 후 검사하므로 함께 걸립니다.
  2. `realpath`로 심볼릭 링크까지 따라간 실제 경로가 루트로 시작하는지 확인합니다.
- **Without**: 1번만 있으면 루트 안의 심볼릭 링크(`www/escape → /etc/passwd`)로 빠져나갈 수 있습니다.
- **Code**:
  - `src/static_file.c` `decode_target_path()`, `has_parent_segment()`, `static_file_open()`의 `realpath` 검사
  - 테스트: `test_static.c`(`..`, 인코딩된 `..`, 심볼릭 링크, `%00`)

**확인 질문**
- 디코딩 **전에** `..`를 검사하면 어떤 우회가 가능한가?
- `%00`을 400으로 거절하는 이유는?

---

## 9. 동시성 모델: 스레드 풀 (그리고 fork / 이벤트 루프와의 비교)

- **What**: 미리 만든 작업 스레드 N개가 공유 작업 큐에서 연결을 꺼내 처리하는 구조입니다. 생산자는 main 스레드, 소비자는 작업 스레드입니다.
- **Why**: 연결을 하나씩 순서대로 처리하면, 느린 클라이언트 하나가 모두를 기다리게 합니다(head-of-line blocking). 요청마다 스레드를 새로 만들면 생성 비용이 들고 개수에 상한이 없습니다.
- **How**:
  - `queue_push`와 `queue_pop`은 **mutex**로 큐를 보호합니다.
  - 큐가 비면 작업 스레드가 `pthread_cond_wait(not_empty)`에서 잠듭니다. CPU를 쓰지 않고, lock을 풀었다가 깨면 다시 잡습니다.
  - 큐가 차면(64) main이 `not_full`에서 기다립니다(**배압**). 그동안 새 연결은 커널 listen 대기열에 쌓입니다.
  - `while` 조건으로 기다리는 이유는 가짜 깨어남(spurious wakeup)에 대비하기 위해서입니다.
- **Without**:
  - mutex가 없으면 두 스레드가 같은 fd를 꺼내거나 count가 깨집니다.
  - 조건 변수가 없으면 큐를 계속 확인하느라 CPU 100%를 씁니다(busy-wait).
- **비교 (커리큘럼 중점 포인트)**:

  | 모델 | 동시 연결당 비용 | 장점 | 단점 |
  |---|---|---|---|
  | process-per-request (`fork`) | 프로세스 1개 | 격리(하나가 죽어도 나머지 무사) | 생성·메모리 비용 큼, 공유 상태 어려움 |
  | **thread pool (우리)** | 스레드 1개 (연결이 끝날 때까지 점유) | 블로킹 코드 그대로, 이해 쉬움 | 동시 연결 수 ≤ 스레드 수 |
  | event loop (`epoll`/`kqueue`) | 상태 구조체 1개 | 스레드 몇 개로 수만 연결(C10K) | 논블로킹 상태 기계 → 코드 복잡 |

- **C10K와 우리 측정값**:
  - keep-alive 연결 50개에 스레드 8개면 처리량이 1,304 요청/초로 떨어지고, 가장 느린 요청은 15초 걸립니다.
  - 스레드를 64개로 늘리면 56,596 요청/초입니다.
  - 즉 스레드 풀은 "연결 하나 = 스레드 하나"라서, 연결 1만 개를 받으려면 스레드 1만 개(스택 메모리, 컨텍스트 스위칭)가 필요합니다. 이것이 C10K 문제이고, 이벤트 루프가 이를 해결하는 방식입니다.
- **Code**:
  - `src/server.c` `queue`(구조체), `queue_push()`, `queue_pop()`, `worker_main()`, `start_workers()`
  - 테스트: `test_concurrent_slow_client_does_not_block_others`, `test_concurrent_many_clients`, `test_pool_exhausted_requests_wait_not_fail`

**확인 질문**
- `pthread_cond_wait`는 왜 mutex를 인자로 받는가?
- 큐가 가득 찼을 때 즉시 503을 보내고 close하던 초기 구현이 왜 실패했는가? (힌트: 읽지 않은 데이터가 남은 소켓을 close하면 RST)
- 스레드 8개 서버에 keep-alive 클라이언트 50개가 붙으면 왜 느려지는가? 해결책 세 가지는?

---

## 10. Reverse Proxy vs Forward Proxy, Round-Robin

- **What**:
  - **Forward Proxy**는 *클라이언트*를 대신합니다. 클라이언트가 프록시를 지정하고, 프록시가 외부 서버에 대신 접속합니다(회사 프록시, 캐시).
  - **Reverse Proxy**는 *서버*를 대신합니다. 클라이언트는 프록시가 진짜 서버라고 생각하고, 프록시가 내부 백엔드 중 하나에 전달합니다(nginx 앞단).
- **Why (Reverse)**:
  - **보안**: 백엔드를 외부에 직접 노출하지 않습니다.
  - **로드 밸런싱**: 여러 백엔드에 나눕니다.
  - **장애 격리**: 죽은 백엔드를 건너뜁니다.
  - **캐싱**: 앞단에서 응답을 재사용할 수 있습니다(우리는 구현하지 않음).
- **How (우리 구현)**:
  1. `-R /api/=A,B` 규칙 중 경로와 **가장 길게** 일치하는 규칙을 고릅니다. 없으면 정적 파일입니다.
  2. **Round-Robin**: mutex 안에서 순번을 하나씩 넘겨 A, B, A, B 순으로 고릅니다. 연결에 실패하면 다음 백엔드를 시도하고, 모두 실패하면 **502 Bad Gateway**입니다.
  3. 요청을 다시 만듭니다. 원래 헤더는 유지하되 **hop-by-hop 헤더**(Connection, Keep-Alive, Upgrade 등)는 빼고, `Connection: close`를 붙입니다.
  4. 백엔드 응답을 받는 대로 클라이언트에 중계합니다. 첫 바이트가 5초 안에 오지 않으면 **504 Gateway Timeout**입니다.
- **Without**:
  - hop-by-hop 헤더를 그대로 넘기면 클라이언트↔프록시 구간의 연결 설정이 프록시↔백엔드 구간에 잘못 적용됩니다.
  - 백엔드 타임아웃이 없으면 멈춘 백엔드 하나가 작업 스레드를 모두 묶습니다.
- **Code**:
  - `src/proxy.c` `proxy_add_route()`, `proxy_find_route()`, `proxy_next_backend()`, `is_hop_by_hop()`, `proxy_build_request()`, `proxy_forward()`
  - `src/server.c` `handle_request()`(분기)
  - 테스트: `test_proxy.c`, 기능 테스트 `test_proxy_*` 8개

**확인 질문**
- "누구의 입장에서 누구를 대신하는가"로 forward와 reverse를 한 문장씩 설명해 보라.
- 502와 504는 각각 어떤 상황인가? 우리 코드에서 504가 나는 정확한 조건은?
- Round-Robin 순번 증가에 mutex가 없으면 어떤 일이 생기는가?

---

## 11. 메모리와 자원 관리

- **What**: `malloc`/`free`, `open`/`close`, `socket`/`close`, `pthread_create`/`pthread_join`의 짝을 정상 경로와 **모든 오류 경로**에서 맞추는 것입니다.
- **Why**: 서버는 오래 실행됩니다. 요청마다 몇 바이트나 fd 하나만 새어도 결국 메모리나 fd가 고갈되어(`EMFILE`) 멈춥니다.
- **How (우리 코드의 규칙)**:
  - **연결 버퍼는 함수 하나가 맡습니다.** `handle_connection`이 `malloc`하고, 모든 `break`가 함수 끝의 `free` 한 곳으로 모입니다.
  - **fd는 연 쪽이 닫습니다.** client fd는 `worker_main`이, 파일은 `serve_static_file`이, 백엔드 fd는 `proxy_forward`가 닫습니다.
  - **크기 상한을 먼저 검사한 뒤에 복사합니다.** `copy_field`, `snprintf` 반환값 검사, Content-Length 곱셈 전 검사가 그 예입니다.
  - **본문은 복사하지 않고 빌립니다.** `request.body`는 버퍼 안을 가리키는 포인터라서, 버퍼가 살아 있는 동안만 씁니다.
- **Without**: 클라이언트가 중간에 끊어 오류 경로로 빠질 때마다 버퍼와 fd가 새게 됩니다.
- **검증**:
  - Linux LSan: 835 요청 혼합 트래픽에서 누수 보고 0건. 대조군으로 넣은 77바이트 누수는 검출했습니다.
  - macOS leaks: 0 leaks.
  - 부하 전후 fd 수: 7 → 7.
- **Code**:
  - `src/server.c` `handle_connection()`, `serve_static_file()`, `worker_main()`
  - `src/static_file.c` `realpath(path, NULL)` → `free(real_path)`
  - `src/proxy.c` `proxy_forward()` 끝의 `close_socket(backend_fd)`

**확인 질문**
- `handle_connection` 안에서 `return`을 쓰지 않고 `break`로 끝까지 내려가는 이유는?
- `request.body`를 저장해 두었다가 다음 요청을 처리한 뒤에 쓰면 왜 위험한가? (`memmove`로 버퍼 내용이 바뀜)

---

## 12. 시그널: SIGINT/SIGTERM 안전한 셧다운, SIGPIPE 무시

- **What**:
  - SIGINT(Ctrl+C)와 SIGTERM(`kill`)은 종료 요청입니다.
  - SIGPIPE는 상대가 닫은 소켓에 쓸 때 커널이 보내는 시그널이고, 기본 동작은 프로세스 종료입니다.
- **Why**:
  - 처리 중인 응답을 끝내고, 자원을 정리하고, 정상 종료 코드(0)로 끝내기 위해서입니다.
  - 클라이언트 하나가 끊었다고 서버 전체가 죽으면 안 됩니다.
- **How**:
  - 핸들러에서는 `printf`나 `malloc`을 쓰면 안 되므로(async-signal-safe가 아님) `atomic_store(&stop_requested, 1)`만 합니다.
  - 플래그 타입은 `volatile sig_atomic_t`가 아니라 C11 `atomic_int`입니다. `sig_atomic_t`는 핸들러와 **같은 스레드** 사이에서만 안전한데, 이 플래그는 작업 스레드도 읽기 때문에 ThreadSanitizer가 데이터 레이스로 보고했습니다. lock-free 원자형은 시그널 핸들러에서도 안전하므로 `_Static_assert(ATOMIC_INT_LOCK_FREE == 2)`로 보장합니다.
  - `SA_RESTART`를 주지 않아서, 블로킹 중인 `poll`이 `EINTR`로 깨어납니다.
  - 작업 스레드에서는 SIGINT/SIGTERM을 `pthread_sigmask`로 막아서, 시그널이 항상 main 스레드로 갑니다.
  - main은 루프를 빠져나와 리스너를 닫고(새 연결 중단), 큐에 `closing`을 표시해 잠든 스레드를 모두 깨운 뒤, `pthread_join`으로 처리 중인 연결이 끝나기를 기다립니다.
  - 처리 중이던 요청은 `Connection: close`를 붙여 끝까지 응답합니다.
  - SIGPIPE는 `SIG_IGN`으로 무시해서, `send`가 `EPIPE` 오류를 돌려주게 하고 그 연결만 닫습니다.
- **Without**:
  - 셧다운 처리가 없으면 Ctrl+C 순간 응답이 반쯤 잘립니다.
  - SIGPIPE를 무시하지 않으면, 응답 도중 브라우저 탭을 닫는 것만으로 서버가 종료됩니다.
- **Code**:
  - `src/server.c` `on_stop_signal()`, `install_signal_handlers()`, `start_workers()`(시그널 마스크), `stop_workers()`, `main()`의 `poll` 루프
  - `handle_connection()`의 `!atomic_load(&stop_requested)`
  - 테스트: `test_graceful_shutdown_finishes_inflight_request`(SIGTERM과 SIGINT 각각), `test_sudden_client_disconnect`

**확인 질문**
- 시그널 핸들러 안에서 바로 `close(listener)`와 `free`를 하면 안 되는 이유는?
- main이 `accept()`에서 바로 블로킹하지 않고 `poll(1초)`를 먼저 쓰는 이유는?
- 셧다운이 최대 몇 초 걸릴 수 있는가? 그 이유는? (keep-alive 유휴 타임아웃)

---

## 마무리: 30초 요약

> 커널이 handshake를 끝낸 연결을 main이 `accept`해서 큐에 넣으면, 작업 스레드가 꺼내 `recv`로 바이트를 쌓습니다. 파서가 `\r\n\r\n`과 `Content-Length`로 요청 경계를 찾으면, 경로 규칙에 따라 백엔드로 전달하거나(Reverse Proxy, Round-Robin, 502/504) 루트 안의 파일을 MIME 타입과 함께 보냅니다(403/404). keep-alive면 같은 연결로 다음 요청을 받고, 끝나면 버퍼를 `free`하고 fd를 `close`합니다. SIGTERM이 오면 새 연결을 멈추고 처리 중인 요청을 끝낸 뒤 스레드를 join하고 종료합니다.
