# CSAPP 3판 (한국어판) 11장 네트워크 프로그래밍 — readme 기준 요약

> 기준: `docs/readme.md` 요구사항. 11장 내용을 과제 항목에 매핑.
> 11장에 없는 것(동시성 = 12장, 프록시 = Proxy Lab, RIO = 10장)도 과제에 필요하면 짧게 표시.

---

## 0. 한눈에 매핑

| readme 항목             | 11장 절         | 핵심                                                          |
| --------------------- | ------------- | ----------------------------------------------------------- |
| TCP 소켓 서버             | 11.3, 11.4    | `socket→bind→listen→accept`, `getaddrinfo`, `open_listenfd` |
| HTTP 요청 파싱            | 11.5.3        | 요청 라인 `METHOD URI VERSION`, 헤더, 빈 줄 `\r\n`                  |
| HTTP 응답 생성            | 11.5.3        | 상태 라인 + 헤더 + 빈 줄 + 본문                                       |
| 정적 파일 서빙              | 11.5.2, 11.6  | MIME 타입, `serve_static`, `mmap`/`read`                      |
| 동적 요청 처리              | 11.5.4, 11.6  | CGI: `fork`+`dup2`+`execve`, `QUERY_STRING`                 |
| 오류 응답                 | 11.6          | `clienterror` → 404/403/501                                 |
| 동시 요청 처리              | ❌ (12장)       | Tiny = iterative 서버. 동시성은 12장                               |
| Proxy / Reverse Proxy | ❌ (Proxy Lab) | 11장 클라이언트 쪽 + 서버 쪽 합치면 프록시                                  |
| SIGPIPE / EPIPE       | 11.6 끝 연습문제   | 클라 끊김 시 쓰기 → SIGPIPE, 무시해야 함                                |

---

## 1. 11.1 클라이언트-서버 모델

- 모든 네트워크 앱 = 클라이언트 프로세스 + 서버 프로세스.
- 기본 연산 = **트랜잭션** 4단계:
  1. 클라 → 요청 전송
  2. 서버 → 요청 해석, 자원 조작
  3. 서버 → 응답 전송
  4. 클라 → 응답 처리
- 클라/서버 = **프로세스**. 호스트 아님. 한 호스트에 둘 다 가능.

## 2. 11.2 네트워크

- 호스트 입장에서 네트워크 = 그냥 I/O 장치 (어댑터 → DMA → 메모리).
- LAN(이더넷 세그먼트, 허브) → 브리지 → 라우터로 internet 구성.
- **프로토콜 소프트웨어**가 해결하는 것 2개:
  - 명명법: 호스트 주소 통일 (IP 주소)
  - 전달 기법: 패킷 = 헤더 + 페이로드
- 캡슐화: 데이터 → (internet 헤더) → (LAN 프레임 헤더) → 라우터에서 벗겨서 재포장.

## 3. 11.3 글로벌 IP 인터넷

- 스택: 앱 ↔ 소켓 인터페이스(시스템 콜) ↔ TCP/UDP ↔ IP ↔ 어댑터.
- **IP**: 호스트→호스트, 비신뢰 데이터그램.
- **UDP**: IP + 포트(프로세스→프로세스). 비신뢰.
- **TCP**: IP 위 **신뢰성 있는 양방향 전이중 연결**. 바이트 스트림 → 메시지 경계 없음. (→ 과제에서 헤더 끝 `\r\n\r\n` 직접 찾아야 하는 이유)

### 3.1 IP 주소
- 32비트 부호 없는 정수 `struct in_addr { uint32_t s_addr; }`.
- **네트워크 바이트 순서 = 빅엔디언**. 변환:
  - `htonl / htons` (host→network), `ntohl / ntohs` (network→host)
- 점-십진 표기 변환: `inet_pton` (문자열→바이너리), `inet_ntop` (반대).

### 3.2 도메인 이름
- 사람용 이름 ↔ IP 매핑 = DNS (분산 DB).
- 1:1, N:1, 1:N, 매핑 없음 모두 가능. `localhost` = `127.0.0.1` (루프백).

### 3.3 인터넷 연결
- 연결 = **점대점, 전이중, 신뢰성**.
- 소켓 = 연결의 끝점 = `IP:포트`.
- **소켓 쌍** `(cliaddr:cliport, servaddr:servport)` 가 연결을 유일하게 식별.
- 클라 포트 = 커널이 자동 할당하는 **임시(ephemeral) 포트**.
- 서버 포트 = **잘 알려진 포트** (HTTP 80, SSH 22 …).

## 4. 11.4 소켓 인터페이스 ★ (readme "TCP 소켓 서버")

### 흐름

```
클라이언트                        서버
getaddrinfo                       getaddrinfo
socket                            socket
                                  bind
                                  listen
connect  ───── 연결 요청 ─────▶   accept  (블록)
rio_writen ──────────────────▶   rio_readlineb
rio_readlineb ◀──────────────     rio_writen
close    ──────── EOF ────────▶   rio_readlineb (0 리턴)
                                  close
```

### 4.1 소켓 주소 구조체
- `struct sockaddr_in` (IPv4 전용): `sin_family`, `sin_port`, `sin_addr` — 전부 네트워크 바이트 순서.
**⚠ 정정:** 요약 문서에 "전부 네트워크 바이트 순서"라고 쓴 건 틀림.
`sin_port`, `sin_addr`: 네트워크 순서
`sin_family` (`AF_INET`): **호스트 순서**. 커널만 읽고 네트워크로 안 나감. 변환 안 함.

- `struct sockaddr` = 제네릭. 함수 인자 타입. → `(SA *)` 캐스팅 필요.
- 이유: `void *` 나오기 전 설계.

### 4.2 `socket`
- `socket(AF_INET, SOCK_STREAM, 0)` → 디스크립터 반환. 아직 반쪽(미연결).
- 실전: 인자는 `getaddrinfo` 결과로 채움 → 프로토콜 독립.

### 4.3 `connect` (클라)
- 서버와 연결 시도. **성공 or 에러까지 블록**.
- 성공하면 `clientfd` 로 읽기/쓰기 가능.

### 4.4 `bind` (서버)
- 서버 소켓 주소 ↔ `sockfd` 연결.
- 실패 흔한 원인: 포트 사용 중(`EADDRINUSE`) → `SO_REUSEADDR` 로 TIME_WAIT 재사용.

### 4.5 `listen` (서버)
- 능동 소켓 → **듣기 소켓**으로 변환. `backlog` = 대기 큐 크기 힌트.

### 4.6 `accept` (서버)
- 연결 요청 올 때까지 **블록**. 오면 **새 연결 디스크립터** 반환.
- **듣기 디스크립터 vs 연결 디스크립터** 구분 ★
  - listenfd: 서버 수명 동안 1개. 요청 받는 끝점.
  - connfd: 클라마다 1개. 연결 동안만 존재.
  - 구분 이유 → 동시성 서버 가능(연결마다 프로세스/스레드에 connfd 넘김).

### 4.7 호스트/서비스 변환
- `getaddrinfo(host, service, &hints, &result)` → `addrinfo` **연결 리스트** 반환. 다 쓰면 `freeaddrinfo`. 에러 문자열 `gai_strerror`.
- 주요 hints:
  - `ai_socktype = SOCK_STREAM` → TCP만
  - `AI_PASSIVE` + host `NULL` → 서버용 와일드카드 주소
  - `AI_ADDRCONFIG` → 로컬 설정에 맞는 주소(IPv4/IPv6)만
  - `AI_NUMERICSERV` → service를 포트 번호로 강제
- `getnameinfo` = 반대 방향 (소켓 주소 → 호스트/서비스 문자열). `NI_NUMERICHOST`, `NI_NUMERICSERV`.
- 장점: **재진입 가능 + 프로토콜 독립(IPv4/IPv6)**. 옛 `gethostbyname` 대체.

### 4.8 도움 함수
- `open_clientfd(host, port)`:
  `getaddrinfo` 결과 리스트 순회 → 각각 `socket` + `connect` → 성공하면 break, 실패면 `close` 후 다음. 끝나면 `freeaddrinfo`.
- `open_listenfd(port)`:
  `AI_PASSIVE|AI_ADDRCONFIG|AI_NUMERICSERV` → 리스트 순회 → `socket` → `setsockopt(SO_REUSEADDR)` → `bind` 성공 시 break → `listen`.
- 포인트: 리스트 순회 = 한 주소 실패해도 다음 시도. `SO_REUSEADDR` = 서버 재시작 즉시 가능.
- 프록시에서 원 서버 붙을 때 = `open_clientfd` 패턴 그대로.

### 4.9 echo 서버/클라 예제
- 서버: `while(1) { connfd = accept(); echo(connfd); close(connfd); }` → **반복(iterative) 서버**, 한 번에 클라 1개.
- 클라가 `close` → 서버 `rio_readlineb` 가 0(EOF) 리턴 → 루프 탈출.
- **EOF 개념** ★: EOF 문자 없음. 커널이 "더 읽을 거 없음" 상태를 0 리턴으로 알림. 소켓에선 상대가 연결 닫을 때.

## 5. 11.5 웹 서버 ★ (readme "HTTP 요청 파싱/응답 생성")

### 5.1 웹 기초
- HTTP = 텍스트 기반 앱 계층 프로토콜. 브라우저 ↔ 서버.
- 컨텐츠 = MIME 타입 붙은 바이트 열.

### 5.2 웹 컨텐츠
- MIME 예: `text/html`, `text/plain`, `image/gif`, `image/png`, `image/jpeg`, `application/postscript`.
- 제공 방식 2가지:
  - **정적 컨텐츠**: 디스크 파일 그대로 반환.
  - **동적 컨텐츠**: 실행 파일 돌리고 그 출력 반환.
- URL 구조: `http://host:port/path?args`
  - 클라는 `host:port` 로 서버 찾음 → 서버는 **URI**(`/path?args`)만 받음.
  - `?` 앞 = 파일 경로, 뒤 = 인자(`&` 구분).
  - 정적/동적 구분 규칙은 서버 마음 (Tiny: `cgi-bin` 포함이면 동적).
  - `/` → 기본 페이지(`index.html`) 로 확장.

### 5.3 HTTP 트랜잭션
**요청**
```
GET /index.html HTTP/1.1\r\n      ← 요청 라인: method URI version
Host: www.example.com\r\n         ← 요청 헤더 header-name: header-data
\r\n                              ← 빈 줄 = 헤더 끝
```
- 메서드: GET, POST, OPTIONS, HEAD, PUT, DELETE, TRACE.
- `Host` 헤더 = HTTP/1.1 필수. **프록시 체인**에서 원 서버 식별용 ★.

**응답**
```
HTTP/1.1 200 OK\r\n               ← 상태 라인: version status-code status-msg
Content-Type: text/html\r\n
Content-Length: 1234\r\n
\r\n
<본문>
```
- 상태 코드: `200 OK`, `301 Moved`, `400 Bad request`, `403 Forbidden`, `404 Not found`, `501 Not implemented`, `505 HTTP version not supported`.
- `Content-Length` = 본문 바이트 수 → 클라가 본문 경계 판단 (readme "Content-Length 기반 본문 처리").

### 5.4 동적 컨텐츠 (CGI)
- 인자 전달 방법:
  - GET: URI의 `?` 뒤 → 서버가 환경변수 `QUERY_STRING` 에 넣음.
  - POST: 요청 **본문**으로 전달 (책은 GET만 다룸 → 과제 POST는 직접).
- 서버 → 자식 실행:
  - `fork` → 자식이 `setenv("QUERY_STRING", ...)` → `dup2(connfd, STDOUT)` → `execve(CGI)`.
  - CGI는 `printf` 로 stdout 쓰면 곧장 클라로 감.
- CGI 출력 담당: `Content-Type`, `Content-Length` 헤더 + 빈 줄 **CGI가 직접 씀**. 서버는 상태 라인 정도만.
- 기타 환경변수: `SERVER_PORT`, `REQUEST_METHOD`, `REMOTE_HOST`, `REMOTE_ADDR`, `CONTENT_TYPE`, `CONTENT_LENGTH`.

## 6. 11.6 Tiny 웹 서버 ★ (readme 거의 전 항목의 원형)

### 구조
| 함수                 | 역할                                                              | readme 매핑        |
| ------------------ | --------------------------------------------------------------- | ---------------- |
| `main`             | `open_listenfd` → 무한 `accept` → `doit` → `close`                | TCP 소켓 서버        |
| `doit`             | 요청 라인 읽기 → 메서드 검사 → 헤더 읽기 → URI 파싱 → `stat` → 정적/동적 분기          | HTTP 요청 파싱       |
| `clienterror`      | HTML 에러 본문 + 상태 라인/헤더 생성                                        | 404/403/501 응답   |
| `read_requesthdrs` | 빈 줄(`\r\n`) 나올 때까지 헤더 줄 읽고 버림                                   | 헤더 파싱 (Tiny는 무시) |
| `parse_uri`        | `cgi-bin` 유무로 정적/동적 판단, filename·cgiargs 추출                     | URI → 파일 경로      |
| `serve_static`     | 헤더 전송 → `open` → `mmap` → `rio_writen` → `munmap`               | 정적 파일 서빙         |
| `get_filetype`     | 확장자 → MIME                                                      | MIME 매핑          |
| `serve_dynamic`    | `200 OK` 보냄 → `fork` → `setenv` → `dup2` → `execve` → 부모 `wait` | 동적 요청 처리         |

### doit 체크 순서
1. 요청 라인 `sscanf` → method/uri/version.
2. GET 아니면 → `501 Not Implemented`.
3. 헤더 읽기.
4. `parse_uri` → 정적/동적.
5. `stat` 실패 → `404`.
6. 정적인데 일반파일 아님 or 읽기권한 없음 → `403`.
7. 동적인데 일반파일 아님 or 실행권한 없음 → `403`.

### Tiny 한계 (= 과제에서 보완할 것)
- **반복 서버** → 동시 처리 X. (12장: 프로세스/스레드/I/O 멀티플렉싱)
- GET만. POST·HEAD X. (연습문제 11.11: HEAD 추가)
- 헤더 무시 → `Content-Length`, `Connection: keep-alive` 처리 X.
- 연결당 요청 1개 후 close.
- 경로 정규화 X → `..` 디렉터리 탈출 위험.
- **SIGPIPE / EPIPE 미처리** (연습문제 11.13): 클라가 먼저 끊으면 쓰기 때 SIGPIPE → 프로세스 죽음. → `signal(SIGPIPE, SIG_IGN)` + `EPIPE` 처리.
- `mmap` 대신 `malloc` + `rio_readn` 연습문제 11.9 → 메모리 해제 짝 맞추기.

## 7. 연습문제 체크 (과제 연관)

| 번호    | 내용                            | 과제 연관               |
| ----- | ----------------------------- | ------------------- |
| 11.6  | Tiny 수정해서 요청 라인·헤더 그대로 에코     | 파서 디버깅              |
| 11.7  | MPG 비디오 MIME 추가               | MIME 매핑 확장          |
| 11.8  | CGI 자식 회수를 `SIGCHLD` 핸들러로     | 좀비 방지, 시그널          |
| 11.9  | `mmap` → `malloc`+`rio_readn` | 메모리 관리              |
| 11.10 | HTML 폼 → CGI adder            | 동적 처리               |
| 11.11 | HEAD 메서드                      | 메서드 확장              |
| 11.12 | POST 메서드                      | readme 필수 POST      |
| 11.13 | 조기 종료 시 SIGPIPE/EPIPE 처리      | readme "SIGPIPE 무시" |

## 8. 11장 밖이지만 readme가 요구 → 참고처

- **RIO (10장)**: `rio_readlineb`/`rio_writen`. 짧은 카운트(short count) 처리. 소켓 읽기는 요청 다 안 올 수 있음 → 반복 읽기 필수.
- **동시성 (12장)**: 프로세스 기반(`fork`, 자식 `close(listenfd)`, 부모 `close(connfd)`), I/O 멀티플렉싱(`select`, 이벤트 루프), 스레드 기반(`pthread_create`, `detach`, connfd를 `malloc` 해서 넘기기 → race 방지), 생산자-소비자 스레드 풀(`sbuf`).
- **Proxy Lab**: 클라 요청 받기(서버 역할) + `open_clientfd` 로 원 서버 연결(클라 역할) + `Host`/`Connection: close`/`Proxy-Connection: close` 헤더 재작성 + 캐시(LRU, 읽기-쓰기 락).

## 9. 한 줄 정리

- 소켓 = 파일 디스크립터. 네트워크 I/O = 파일 I/O.
- listenfd 1개 / connfd 클라마다.
- `getaddrinfo` 리스트 순회 = 프로토콜 독립 + 실패 내성.
- TCP = 바이트 스트림 → HTTP 경계는 `\r\n`, `\r\n\r\n`, `Content-Length` 로 직접 자름.
- Tiny = 정적/동적/에러 뼈대. 과제 = Tiny + 동시성 + POST + keep-alive + 프록시 + 안전장치.
