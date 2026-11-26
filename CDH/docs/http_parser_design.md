# 2단계: HTTP 요청 파싱 설계

## 목표 (readme 요구사항)

- 요청 라인 파싱: 메서드, URI, 버전
- 헤더 파싱: `Host`, `Content-Length`, `Connection` 등
- 본문(body) 처리: POST
- 최소 지원 메서드: `GET`, `POST`
- 엣지 케이스: 불완전한 헤더, 비정상 메서드, 잘못된 버전, 매우 긴 URI, 매우 큰 본문
- 단위 테스트로 파서 검증

## 시작 시점 상태 (1단계 TCP 서버)

- `tcp_recv_request()`: `\r\n\r\n` 까지 수신, `header_len` 반환. 본문은 안 받음
- `handle_client()`: 헤더 끝 여부만 보고 고정 200 / 400 / 431 응답
- 수신 버퍼 8KB, 연결 하나씩 순차 처리

## 구현 단계

| 단계 | 내용 | 상태 |
|---|---|---|
| 1 | 파서 모듈 `http_parser.h/.c` | 완료 |
| 2 | 본문 수신 (`Content-Length` 만큼, 상한 초과 413) | 완료 |
| 3 | `main.c` 연결: 파싱 결과로 응답 분기 (echo 응답) | 완료 |
| 4 | 단위 테스트 `tests/` + `make test` | 완료 |

keep-alive 는 이번 단계에서 **파싱만** 한다. 실제 연결 재사용은 다음 단계.

---

## 1단계: 파서 모듈

### 파일

- `include/http_parser.h` — 구조체, 함수 선언
- `src/http_parser.c` — 구현
- `Makefile` — `SRCS` 에 추가, 헤더 의존성을 `$(wildcard include/*.h)` 로 변경

### 방식: 제자리(in-place) 파싱

- 버퍼 복사 없음. 구분자(공백, `:`, CR) 자리에 `'\0'` 을 써서 문자열을 끊는다.
- 구조체는 버퍼 안을 가리키는 포인터만 저장 → `malloc`/`free` 없음.
- 대가: `req` 의 문자열은 `buf` 가 살아 있는 동안만 유효.
- 소켓과 무관한 순수 함수 → 단위 테스트가 쉬움.

### API

```c
int http_parse_request(char *buf, size_t header_len, http_request *req);
const char *http_get_header(const http_request *req, const char *name); /* 대소문자 무시 */
```

반환값: `0` = 성공, 그 외 = 클라이언트에게 보낼 HTTP 상태 코드.

### 구조체

```c
typedef struct {
    http_method method;       /* GET / POST / OTHER */
    const char *method_str;
    const char *uri;
    int version_minor;        /* 1.0 → 0, 1.1 → 1 */
    http_header headers[64];  /* {name, value} */
    size_t header_count;
    const char *host;         /* 없으면 NULL */
    int has_content_length;
    size_t content_length;
    int keep_alive;
    const char *body;         /* 2단계에서 채움 */
    size_t body_len;
} http_request;
```

### 상수

| 이름 | 값 | 초과 시 |
|---|---|---|
| `HTTP_MAX_HEADERS` | 64 | 431 |
| `HTTP_MAX_URI_LEN` | 2048 | 414 |

### 파싱 순서

1. 사전 검사: 헤더가 `\r\n\r\n` 으로 끝나는지, 중간에 NUL 바이트가 없는지
2. 요청 라인: `메서드 SP URI SP 버전 CRLF`
3. 헤더 줄: 빈 줄이 나올 때까지 `이름 ":" OWS 값 OWS CRLF`
4. 의미 검사: Host / Content-Length / Transfer-Encoding / Connection 추출
5. 최종 판정: Host 필수 여부, 501 대상, keep-alive 결정

문법 오류(400)를 먼저 걸러 내고, 그다음 505와 501 순서로 판정한다.

### 오류 코드 규칙

**원칙: 애매하면 거절.** 느슨한 파서는 프록시 앞뒤에서 해석이 달라지는 요청 스머글링의 원인이 된다.

| 상황 | 코드 |
|---|---|
| 단독 LF / 단독 CR 줄바꿈 | 400 |
| 헤더 안 NUL 바이트 | 400 |
| 요청 라인 공백 개수 이상 (`GET  /`, 끝에 공백) | 400 |
| 메서드에 token 이외 문자 | 400 |
| URI 가 `/` 또는 `http://` 로 시작 안 함, 제어 문자 포함 | 400 |
| URI 2048바이트 초과 | 414 |
| 버전 형식이 `HTTP/d.d` 아님 (`HTTX/1.1`, 소문자 `http`) | 400 |
| 형식은 맞지만 1.0 / 1.1 아님 (`HTTP/2.0`) | 505 |
| 헤더 이름이 비었거나 token 아님 (`Host :` 처럼 콜론 앞 공백) | 400 |
| 콜론 없는 헤더 줄 | 400 |
| obs-fold (공백으로 시작하는 줄) | 400 |
| 헤더 값에 제어 문자 (HTAB 제외) | 400 |
| 헤더 64개 초과 | 431 |
| Host 중복 | 400 |
| HTTP/1.1 인데 Host 없음 | 400 |
| Content-Length 가 숫자가 아님 / 음수 / 오버플로 | 400 |
| Content-Length 중복 + 값이 다름 (같으면 허용) | 400 |
| Transfer-Encoding 존재 (chunked 미지원) | 501 |
| GET / POST 이외 메서드 (`PUT`, 소문자 `get`) | 501 |

### keep-alive 결정

| 버전 | Connection 헤더 | 결과 |
|---|---|---|
| 1.1 | 없음 / `keep-alive` | 유지 |
| 1.1 | `close` 포함 | 종료 |
| 1.0 | 없음 | 종료 |
| 1.0 | `keep-alive` | 유지 |
| any | `close` 포함 | 종료 (최우선) |

- `Connection: Upgrade, Close` 같은 쉼표 목록 값을 토큰 단위로 비교 (대소문자 무시)
- Connection 헤더가 여러 줄이면 전부 검사

### 메서드 / 버전 대소문자

- 메서드: 대소문자 구분 (RFC 7231 4.1) → `get` 은 501
- `HTTP` 문자열: 대소문자 구분 (RFC 7230 2.6)
- 헤더 이름: 대소문자 무시

### 검증 결과

WSL에서 `make asan` 빌드, 경고 0개. 임시 스모크 테스트 24개 통과 (위 표의 케이스들).

---

## 2단계: 본문 수신

### 파일

- `tcp_server.h/.c` — `tcp_recv_exact()` 추가: 정확히 n 바이트를 받을 때까지 반복 수신 (바이트만 다룸, HTTP 모름)
- `include/http_body.h`, `src/http_body.c` — `http_read_body()`: Content-Length 를 보고 본문 완성
- `Makefile` — `SRCS` 에 `src/http_body.c` 추가

역할 분리: 파서는 소켓을 모르는 순수 함수로 유지하고, 소켓이 필요한 본문 수신은 별도 모듈로 둔다.

### 버퍼 상태

`tcp_recv_request` 는 헤더 끝까지만 기다리지만, 한 번의 recv 에 본문 일부가 딸려 오는 일이 흔하다.

```
buf: [ 헤더 ... \r\n\r\n | 본문 앞부분(have) | 아직 안 온 부분 ... ]
      0          header_len           received
```

### API

```c
#define HTTP_MAX_BODY_SIZE (1024 * 1024)
int http_read_body(int fd, http_request *req, char *buf, size_t cap,
                   size_t received, size_t header_len, int timeout_ms, char **heap_out);
```

| 반환 | 의미 | 호출자 처리 |
|---|---|---|
| 0 | 성공. `req->body`, `req->body_len` 채워짐 | 응답 후 `free(heap)` |
| -1 | 타임아웃(`ETIMEDOUT`) / 도중 끊김(`ECONNRESET`) | 응답 없이 닫기 |
| 411 | POST 인데 Content-Length 없음 | 오류 응답 |
| 413 | 본문이 1MB 초과 | 오류 응답 |
| 417 | `Expect` 값이 `100-continue` 가 아님 | 오류 응답 |
| 500 | malloc 실패 | 오류 응답 |

### 처리 순서

1. `Expect` 확인 (HTTP/1.1 만): `100-continue` 면 표시, 다른 값이면 417
2. Content-Length 없음: POST 면 411, 그 외는 본문 없음으로 성공
3. Content-Length 가 1MB 초과 → 413 (100 Continue 보내기 **전**에 판정 → 클라이언트가 큰 본문을 헛되이 보내지 않음)
4. 0 이면 빈 본문
5. 이미 다 받았으면(`have >= cl`) 버퍼 안을 그대로 가리킴. `cl` 뒤의 남는 바이트는 다음 요청(파이프라이닝)이라 본문에 넣지 않음
6. 본문을 아직 하나도 못 받았고 `Expect: 100-continue` 면 `HTTP/1.1 100 Continue` 전송 (curl 은 1KB 넘는 POST 에서 이걸 기다림)
7. 담을 곳 결정: 헤더 버퍼에 `본문 + '\0'` 자리가 있으면 이어 받기 (malloc 없음), 없으면 `malloc(cl + 1)` 후 앞부분 복사
8. `tcp_recv_exact` 로 남은 `cl - have` 바이트만 정확히 수신 (다음 요청을 먹지 않게). 실패하면 힙 반납 후 -1
9. 본문 끝에 `'\0'` (로그용, 진짜 길이는 `body_len`)

### 검증 결과

WSL에서 빌드 경고 0개. socketpair + fork 테스트 13개를 asan/ubsan 으로 돌려서 모두 통과:
본문 없는 GET / 첫 recv 에 본문이 다 옴 / 본문이 나중에 옴 / 파이프라이닝 잔여 바이트 / CL 0 / 411 / 413 / 417 /
100-continue 교환 / 작은 버퍼에서 힙 경로 / 도중 끊김(-1, ECONNRESET) / 정지(-1, ETIMEDOUT) / 정확히 1MB.

## 3단계: main.c 연결

### 파일

- `include/http_response.h`, `src/http_response.c` — 순수 함수 (소켓 모름)
  - `http_reason(status)`: 상태 코드 → 이유 문구 표 (100, 200, 400, 403, 404, 405, 408, 411, 413, 414, 417, 431, 500, 501, 502, 504, 505). 모르는 코드는 `"Unknown"`
  - `http_format_head(out, cap, status, content_type, content_length, keep_alive)`: 상태 줄 + `Content-Type` / `Content-Length` / `Connection` + 빈 줄. 버퍼가 모자라면 -1 (잘린 응답을 보내지 않음)
- `src/main.c` — 고정 응답 상수(`RESP_200/400/431`)를 지우고 아래 흐름으로 교체

### handle_client 흐름

```
tcp_recv_request ─ 실패/빈 연결 ──────────────→ 응답 없이 close
       │ 헤더 끝 못 찾음 → 버퍼 가득: 431 / 도중 끊김: 400
       ▼
요청 라인 로그 (파싱이 '\0' 을 쓰기 전에, 최대 200자)
       ▼
http_parse_request ─ rc != 0 ────────────────→ send_error(rc)
       ▼
http_read_body ─ -1 ─────────────────────────→ 응답 없이 close
       │       └ 411/413/417/500 ────────────→ send_error(rc)
       ▼
send_echo → free(heap)
```

- `send_error(status)`: 본문 `"<코드> <이유>\n"`, `Connection: close`, 머리와 본문을 한 번에 전송
- `send_echo(req)`: 파싱 결과 요약(메서드, URI, 버전, Host, 헤더 수, keep-alive, 본문 길이) + 빈 줄 + 받은 본문 원문. 정적 파일, 프록시 단계에서 이 자리를 대체할 예정
- 응답은 아직 항상 `Connection: close` (keep-alive 는 파싱만 하고, 연결 재사용은 다음 단계)

### 기능 테스트 결과 (WSL, asan 빌드, curl / nc)

| 요청 | 결과 |
|---|---|
| `curl /hello?x=1` | 200 + 요약 |
| `curl -d 'name=jungle'` | 200, 본문 그대로 돌려줌 |
| 5000바이트 POST + `Expect: 100-continue` | 200, `body-length: 5000` |
| 2MB POST | 413 |
| `PUT` | 501 |
| HTTP/1.1, Host 없음 | 400 |
| `HTTP/2.0` | 505 |
| `Host : a` | 400 |
| POST, CL 없음 | 411 |
| `Transfer-Encoding: chunked` | 501 |
| `Expect: nope` | 417 |
| HTTP/1.0, Host 없음 | 200 |
| URI 3000바이트 | 414 |
| 본문 도중 끊김 | 응답 없음 (0바이트) |

서버 로그에 ASan / UBSan 오류 0건.

## 4단계: 단위 테스트

### 구조

```
tests/
  test.h                 CHECK / CHECK_INT / CHECK_STR / TEST_REPORT 매크로 (외부 프레임워크 없음)
  test_http_parser.c     파서 (88 checks)
  test_http_body.c       본문 수신, socketpair + fork (36 checks)
  test_http_response.c   이유 문구 / 응답 머리 (16 checks)
```

- 실패해도 멈추지 않고 `파일:줄: FAIL: 식 == 실제값 (expected 기대값)` 을 출력, 끝에 요약
- 실패가 하나라도 있으면 종료 코드 1

### make test

```
make test
```

- `tests/test_*.c` 하나가 실행 파일 하나 (`build/tests/test_xxx`). 새 테스트 파일은 자동으로 포함
- 링크: 테스트 .c + `LIB_SRCS` (main.c 를 뺀 모듈 전부)
- 항상 `-fsanitize=address,undefined` 로 빌드 → 메모리 오류도 테스트 실패로 잡힘
- Makefile 에 `LIB_SRCS` 변수를 새로 두고 `SRCS = main.c + LIB_SRCS` 로 정리

### 테스트 케이스

**파서**
- 정상 요청: 필드 추출 (메서드, URI, 버전, Host, CL, 헤더 수), 값의 앞뒤 공백·탭 제거, `http_get_header` 대소문자 무시, absolute-form, 빈 값, UTF-8 값, 값 안의 탭과 `:`
- keep-alive 표 전부 + 쉼표 목록, 여러 줄, `closed` 처럼 부분만 같은 값, close 우선
- 오류 코드 표 전부: 줄바꿈, NUL, 요청 라인, 버전, 메서드, 헤더 줄, Host, Content-Length, Transfer-Encoding
- 판정 순서: Host 없는 `PUT` → 501 이 아니라 400
- 경계값: URI 2048 → 통과 / 2049 → 414, 헤더 64 → 통과 / 65 → 431, CL `SIZE_MAX` → 통과 / 한 자리 더 → 400

**본문 수신**
- 첫 recv 에 다 옴 / 나중에 옴 / 파이프라이닝 잔여 / CL 0 / 본문 없는 GET
- 411 / 413 / 417, HTTP/1.0 의 Expect 는 무시
- 100-continue: 자식이 `HTTP/1.1 100 Continue` 를 실제로 받는지 확인
- 413 이면 100 을 보내지 않음
- 작은 버퍼 → 힙 경로, 본문 안 NUL 바이트, 정확히 1MB
- 도중 끊김 → -1 / `ECONNRESET`, 정지 → -1 / `ETIMEDOUT`

**응답**
- 이유 문구 표, 모르는 코드
- 응답 머리 문자열 전체 비교, keep-alive 표시
- 버퍼 크기: 딱 맞음 / 1바이트 부족 → -1

### 결과

`make test` → 140 checks, 0 failed. `make` 경고 0개.

## 다음 단계 후보

- keep-alive 실제 지원: 한 연결에서 요청 반복 처리, 파이프라이닝 잔여 바이트 이어 쓰기
- 정적 파일 서빙: URI 디코딩 (`%xx`), `..` 경로 탈출 차단, MIME 매핑, 403 / 404
- 동시 처리 모델 선택 (스레드 풀 / epoll)
