# Football HTTP — 경기 페이지로 배우는 C 웹 서버

브라우저에서 `GET /matches/1`로 접속해 응원 메시지를 보내고, 다른 브라우저에서
같은 메시지를 확인하는 C 기반 축구 채팅 프로토타입이다. 화면은 HTML/CSS/JS이며
POST·GET·메모리 저장은 직접 구현한 C HTTP 서버가 처리한다.
Home FC / Away FC는 학습용 가상 경기이며 실제 경기 데이터가 아니다.

기존 순차 TCP 에코 서버의 연결 수락·부분 송신·소켓 정리를 유지하고,
`recv`와 `send_all` 사이에 HTTP 파싱·축구 서비스 라우팅·응답 생성을 추가했다.
이번 단계는 경기 조회 + 채팅 등록·조회 + HTTP Polling이다. WebSocket·실시간 push,
임의 경로의 정적 파일 서빙, 동시성 모델, Proxy, 10,000명 부하 검증은 아직 구현하지 않았다.

## 실행과 직접 확인

C 컴파일러·make·Python 3가 필요하다. Python 표준 라이브러리만으로 `web/match.html`을
C 바이트 배열로 만들어 서버에 포함한다. 외부 HTTP·UI 라이브러리는 사용하지 않는다.
실행 시 HTML 파일의 상대 경로에 의존하지 않도록 빌드 시 포함하는 방식을 선택했다.

```sh
make
./server 8080
```

브라우저에서 <http://127.0.0.1:8080/matches/1>을 연다.
같은 컴퓨터에서만 접속할 수 있도록 기존 루프백 바인딩을 유지했다.
포트를 생략하면 8080이다. 종료는 Ctrl+C다.

```sh
curl -i http://127.0.0.1:8080/matches/1
curl -i http://127.0.0.1:8080/api/matches/1
curl -i http://127.0.0.1:8080/matches/2
curl -i -X POST -H 'Content-Type: text/plain; charset=utf-8' \
  --data-binary '골!!!' http://127.0.0.1:8080/api/matches/1/messages
curl -i http://127.0.0.1:8080/api/matches/1/messages
```

POST는 **201 Created**와 저장된 메시지의 id/text/created_at_ms를 반환한다.
GET은 최근 메시지 목록을 반환한다. 201은 서버 메모리에 저장됐다는 뜻이며 다른 사용자의
화면 표시까지 보장하지 않는다. 두 브라우저 또는 두 탭에서 같은 주소를 열어 전송·조회를 확인한다.

## 사용자 행동과 코드 흐름

```text
브라우저: 경기 페이지 열기
  → GET /matches/1 HTTP/1.1 + Host 헤더
main / create_listener              [server.c]
  → accept → handle_client
  → recv로 바이트를 누적
  → http_parse_request              [http.c]
       NEED_MORE → 추가 recv
       ERROR     → 오류 응답 후 종료
       COMPLETE  → football_route   [football.c]
  → http_response_header            [http.c]
  → send_all로 헤더와 본문 송신
  → main이 client 소켓 close
브라우저: HTML 표시 → 버튼 클릭 → GET /api/matches/1 → JSON 표시
```

- `server.c`: TCP 설정, 누적 수신, 네트워크 오류, 송신과 연결 수명.
- `http.h` / `http.c`: 요청 구조, 파서, 상태 코드, 응답 헤더. 소켓 I/O와 무관하게 단위 테스트 가능.
- `football.h` / `football.c`: 경기 페이지·정보·메시지 POST/GET 라우팅과 Content-Type 정책.
- `chat.h` / `chat.c`: 길이·UTF-8 검증, 최근 100개 링 버퍼, JSON 직렬화. 소켓과 분리.
- `web/match.html`: 채팅 입력·목록·Polling·중지/재개·브라우저 요청 시간.
- `tools/embed_page.py`: HTML을 빌드용 배열로 생성. 생성된 football_page.inc는 직접 수정하지 않는다.
- `tests/test_http.c`: 모든 요청 분할 위치, 바이너리, 크기 경계, 잘못된 프레이밍, 라우팅·응답 생성.
- `tests/test_send.c`: 기존 부분 send / EINTR / EPIPE / timeout 제어 테스트 유지.
- `tests/test_chat.c`: UTF-8·텍스트 검증, 복사/소유권, 보관 상한·ID 순서·JSON escape.
- `tests/test_server.py`: 실제 TCP 요청·응답·채팅 저장·조회·재시작 및 순차 처리 회귀.

## 파서의 계약과 설계 이유

`http_parse_request(data, length, request, consumed, status)`는 현재까지 누적한 바이트를
읽기만 한다. 입력 끝을 C 문자열의 NUL로 찾지 않는다.

| 결과 | 의미 | 호출자의 다음 행동 |
|---|---|---|
| HTTP_NEED_MORE | 헤더 또는 지정한 본문이 아직 부족 | 같은 버퍼 뒤에 추가 수신 |
| HTTP_COMPLETE | 요청 한 개가 완성 | consumed 바이트까지 한 요청으로 처리 |
| HTTP_ERROR | 잘못됐거나 지원하지 않는 요청 | status로 오류 응답 후 연결 종료 |

`request`는 method / target / version / host / connection_close / body / body_length를
제공한다. Connection 헤더에 close 토큰이 있는지는 읽지만, 응답은 해당 값에 관계없이
이번 버전의 **Connection: close** 정책을 따른다.

`body`는 수신 버퍼 내부를 빌린 포인터이며 **복사본이 아니다**. 라우팅과 응답 송신이
끝나기 전 버퍼를 free하거나 옮기면 안 된다. 본문 길이는 바이트 수다. UTF-8 `골!!!`은
6바이트이며, 파서는 NUL 포함 바이너리도 strlen으로 자르지 않는다. 채팅 서비스는
그 위에서 NUL·잘못된 UTF-8 등을 별도로 거부한다.

- 헤더의 끝 `\r\n\r\n`과 Content-Length로 요청 완성을 판단한다. recv 한 번을 요청 하나로 보지 않는다.
- 헤더 최대 16,384바이트(종료 구분자 포함), 요청 대상 최대 2,048바이트, 본문 최대 65,536바이트.
- 본문 길이는 숫자를 누적할 때 상한을 검사하여 정수 오버플로우와 무제한 할당을 피한다.
- 헤더 이름은 대소문자 구분 없이 처리한다. Host 누락·중복·빈 값, 헤더 제어문자,
  잘못된 CRLF, 공백이 섞인 헤더 이름, 중복 Content-Length는 거부한다.
- Host는 길이·공백·위험한 구분자 등을 검사하지만 URI authority 전체 문법 검증기는 아니다.
- 정해진 요청 메서드 저장 공간은 31바이트다. 더 긴 메서드는 400으로 거부한다.
- 현재 HTTP/1.1과 `/`로 시작하는 origin-form만 지원한다. 쿼리는 라우팅에서 제외하고
  퍼센트 디코딩은 하지 않는다. 절대 URL·기타 request-target 형식은 지원하지 않는다.
- 본문이 있는 GET도 길이대로 수신한 뒤 조회한다. POST는 빈 본문이어도 Content-Length를 요구한다.
- Transfer-Encoding과 Content-Length가 함께 있으면 400. Transfer-Encoding만 있어도
  아직 chunked 디코더가 없으므로 501로 거부 후 종료한다.
- Expect는 417로 거부하여 클라이언트와 서버가 서로 100 Continue/body를 기다리지 않게 한다.

이 버전은 **제한된 학습용 HTTP 구현**이며 HTTP/1.1 전체 준수를 주장하지 않는다.
프레이밍 참고: [RFC 9112 §6](https://www.rfc-editor.org/rfc/rfc9112.html#section-6).

## 응답과 오류 정책

| 요청/상황 | 응답 |
|---|---|
| GET / 또는 /matches/1 | 200 + UTF-8 HTML |
| GET /api/matches/1 | 200 + 가상 경기 JSON |
| GET /api/matches/1/messages | 200 + 최근 메시지 목록 |
| POST /api/matches/1/messages | 유효한 text/plain 본문이면 201 + 저장한 메시지 |
| 없는 경기/경로 | 404 |
| 조회 자원에 GET 외 메서드 | 405 + Allow: GET |
| 잘못된 헤더·길이·불완전 요청 EOF | 400 |
| 약 5초간 수신 진행 없음 | 408 후 종료 |
| POST에 Content-Length 없음 | 411 |
| 본문/URI/헤더 상한 초과 | 413 / 414 / 431 |
| 채팅 빈 본문·공백만·잘못된 UTF-8·NUL/금지 제어문자 | 400 |
| 채팅 512바이트 초과 / 미지원 Content-Type | 413 / 415 |
| Expect / Transfer-Encoding 미지원 | 417 / 501 |
| 문법은 맞지만 HTTP/1.1 외 버전 | 505 |

상태 라인, Content-Type, 바이트 단위 Content-Length, Connection: close를 생성한다.
HEAD는 기능 지원 대상이 아니므로 조회 경로에서 405를 반환하되 응답 body는 생략한다.
응답 헤더 생성은 버퍼 부족과 CRLF 혼입도 검사한다. 기존 send_all로 부분 송신과
EINTR을 처리하며 SIGPIPE로 서버가 종료되지 않게 한다.

## 자원 소유권과 한계

- main: listener / client 소켓 소유. handle_client가 반환하면 client를 닫는다.
- handle_client: 요청 누적 버퍼를 할당하고 모든 종료 경로에서 해제한다.
- parser: 요청 body 포인터를 빌리며, 메모리 할당·소켓 처리를 하지 않는다.
- football_route: 정적 응답은 빌려주고 채팅 JSON은 owned_body에 할당한다.
- handle_client: 송신 성공/실패 모두 football_response_release로 owned_body를 해제한다.
- ChatStore: 메시지 텍스트를 자체 배열에 복사한다. 요청 버퍼 해제 뒤에도 유효하다.

연결당 요청 하나만 처리하고 닫는다. keep-alive와 파이프라이닝은 미지원이다.
파서는 consumed로 첫 요청 길이를 알려주지만 서버는 다음 요청을 실행하지 않는다.
잘못된 요청을 일찍 거부할 때 미수신 입력이 남으면 OS가 연결을 reset할 수 있다.

기존 단일 스레드 Blocking 구조다. 첫 사용자가 헤더나 본문을 늦게 보내면 다른 사용자도
대기한다. 수신·송신 타임아웃은 각 blocking I/O 대기 제한이며 **연결 전체 기한이 아니다**.
계속 조금씩 보내는 사용자의 장기 점유 문제는 남아 있다.
SIGINT/SIGTERM의 graceful shutdown은 아직 없다. 테스트 종료 때도 프로세스 기본 종료를 사용한다.

## 검증

```sh
make test
```

C 단위 테스트 3개 프로그램과 Python TCP 기능 테스트 27개를 실행한다. 기능 테스트에는
페이지·JSON, 정상·비정상 헤더, 분할 본문·UTF-8·바이너리, 크기 제한, 타임아웃,
느린 연결의 대기 전파, 100회 조회, 채팅 등록/다른 연결 조회, 100개 보관 후 오래된 항목 교체,
UTF-8·바이너리 거부·JSON escape, 여러 전송자의 ID/본문 정합성, 재시작 시 비우기, 로그가 포함된다.
100회 테스트는 최대 20개 클라이언트 스레드를 사용한 정확성 검사다. 100개 연결을
동시에 처리하거나 10,000명 축구 채팅 성능을 검증했다는 뜻은 아니다.

AddressSanitizer와 UndefinedBehaviorSanitizer 실행 예시:

```sh
make clean
make CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test
make clean
make
```

2026-11-23 macOS / Apple clang에서 일반 빌드의 C 단위 테스트 3개 프로그램과
TCP 기능 테스트 27개가 통과했다. ASan·UBSan 빌드에서도 같은 검사가 통과했다. ASan·UBSan은 잘못된 접근/정의되지
않은 동작 검사이며, 이 환경에서 LeakSanitizer로 누수까지 검사한 것으로 표현하지 않는다.
브라우저 두 탭에서 A 전송→B 조회, Polling 중지/재개, 새로고침 후 내역 유지,
513바이트 입력 차단, HTML 태그의 텍스트 표시를 확인했다.
Linux 동작과 별도 누수 검사는 아직 확인하지 않았다.

## 채팅의 입력·저장 계약

- 경기 1개, 로그인/사용자 식별 없음. 사용자 수나 실제 경기 점수를 만들어 표시하지 않는다.
- `POST /api/matches/1/messages`는 UTF-8 text/plain(생략된 charset은 UTF-8로 해석)을 받는다.
  JSON 요청 본문과 다른 charset은 지원하지 않는다. Content-Type 누락은 415, 빈/중복 헤더는 400.
- 메시지당 512바이트. 빈 입력·ASCII 공백/탭/줄바꿈만 있는 입력·NUL·금지 제어문자·잘못된 UTF-8은 400.
- 최근 100개를 고정 크기 링 버퍼에 보관한다. 가득 차면 가장 오래된 메시지부터 교체한다.
- id는 프로세스 안에서 증가한다. created_at_ms는 서버 벽시계의 Unix epoch 밀리초다.
- 저장소는 프로세스 메모리이며 재시작하면 비워진다. 영속 저장·사용자 인증은 없다.
- JSON escape를 적용하고 브라우저는 textContent로 표시한다. 메시지를 HTML로 실행하지 않는다.
- 메모리 할당 실패나 네트워크 오류로 응답이 유실되면 저장됐어도 클라이언트는 실패로 볼 수 있다.
  중복 방지 키가 없으므로 POST를 자동 재시도하지 않는다. GET 재조회로 확인한다.
- 현재 단일 스레드용이다. Thread Pool 등 도입 시 ChatStore의 추가·조회 범위를 함께 보호해야 한다.

POST 응답 예시(형태 설명용이며 실제 시각/ID 아님):

```json
{"id":1,"created_at_ms":1795430000000,"text":"골!!!"}
```

GET 응답은 `{match_id, capacity, max_text_bytes, messages:[...]}` 형식이다.

## 두 사용자로 관찰하기

1. A와 B에서 `/matches/1`을 연다. 같은 서버 프로세스·포트에 연결해야 한다.
2. A가 메시지를 전송하면 201 저장 완료와 전송 왕복 시간을 확인한다.
3. B는 이전 조회가 끝난 후 1초 뒤에 다시 GET하여 목록을 갱신한다.
4. B의 자동 조회를 중지하고 A가 전송한다. B는 새 목록을 가져오지 않는다.
5. B에서 조회를 재개하거나 페이지를 다시 열면 메모리에 남은 최근 목록을 확인한다.
6. 서버를 재시작하면 목록은 비워진다. 연결이 끊겼다가 서버가 돌아오면 다음 Polling에서 다시 조회한다.

Polling은 응답 완료 후 1초 뒤 재예약한다. 느린 요청이 있어도 조회를 계속 쌓지 않는다.
각 요청의 클라이언트 제한시간은 7초다. 중지 버튼은 이후 조회 예약을 멈추며 이미 진행 중인
조회 하나가 마무리될 수 있다. 요청마다 Connection: close이므로 연결 유지 서비스는 아니다.

## 로그와 측정 지점

완성된 요청마다 `[http] request_id=… method=… target=… status=…`와 다음 값을 기록한다.
메시지 내용 자체는 로그에 남기지 않고 `[chat] stored_id=… bytes=… retained=…`를 남긴다.

| 지표 | 범위와 주의점 |
|---|---|
| receive_ms | accept 후 요청 전체를 수신·파싱할 때까지. 느린 전송 대기 포함 |
| route_ms | 입력 검증·저장/조회·응답 JSON 생성 |
| send_ms | 응답을 send_all에 전달하는 시간. 상대의 읽기 완료 시간이 아님 |
| total_ms | 위 구간 합계. 커널 accept 대기열에서 기다린 시간은 제외 |
| send_ok | 서버 송신 함수의 성공 여부. 다른 사용자의 수신 확인이 아님 |
| 화면의 전송/조회 왕복 | fetch 시작부터 응답 본문 수신·JSON 해석까지. 대기열/네트워크 포함 |

다른 화면 표시 지연에는 Polling 주기·브라우저 스케줄링·탭 비활성화 영향도 있다.
서버의 created_at_ms와 클라이언트 시계의 차이를 네트워크 지연으로 단정하지 않는다.
요청 로그는 부하 측정에도 비용을 더하므로 동시성 모델 비교 시 조건을 맞춘다.

## 이번 선택과 다음 실험

사용자가 채팅을 보냈을 때 다른 화면에서도 보이는 최소 흐름을 먼저 만든다.
HTTP 파서는 재사용하고 서비스 라우터에 등록·조회만 추가했다. bounded 메모리 저장과
Polling은 동작을 관찰하기 위한 초기 선택이며, 10,000명 처리 성능을 달성한 근거가 아니다.
설명을 제공했다는 사실을 사용자의 이해 확인이나 팀 합의로 기록하지 않는다.

다음 실험은 느리게 헤더/본문을 보내는 A가 정상 사용자 B의 조회·등록도 지연시키는지다.
관찰 후 동시성 모델과 저장소 보호를 함께 결정한다. WebSocket·영속 저장·Proxy는 별도 단계다.
