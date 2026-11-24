# MicroHTTP

C11 + POSIX 로 처음부터 만든 HTTP/1.1 서버. 레이어를 아래에서부터 하나씩 쌓았다.
각 레이어는 바로 아래 레이어만 안다.

| 레이어 | 내용 | 파일 |
|---|---|---|
| L1 | TCP 소켓 (listen/accept/recv/send, 절대 마감 시각, graceful close) | `tcp.h` `tcp.c` |
| L2 | HTTP 요청 파싱 (요청 라인, Host/Content-Length/Connection, POST 본문) | `http_request.h` `http_request.c` |
| L3 | HTTP 응답 생성 (상태 줄, 자동 헤더, 메모리/파일 본문, 오류 페이지) | `http_response.h` `http_response.c` |
| L4 | 정적 파일 서빙 (문서 루트, MIME, 301/403/404/405) | `static.*` `mime.*` `handler.h` |
| L5 | 라우터 + CGI/1.1 (GET 쿼리, POST 본문, 502/504) | `router.*` `cgi.*` |
| L6 | 동시성 (keep-alive, 스레드 풀, 대기열 포화 503, 우아한 종료) | `conn.c` `threadpool.*` `server.c` |

```
accept ─▶ 스레드 풀 ─▶ conn_serve (keep-alive 루프)
                         recv_some (L1) → http_parse (L2) → router_dispatch (L5)
                         → static_handle (L4) / cgi_handle (L5) → http_response_send (L3)
```

## 빌드와 실행

```sh
make
./microhttp                 # 0.0.0.0:8080, ./www 와 ./cgi-bin 사용, 워커 8개
./microhttp -q -r www -c cgi-bin -w 16 9000
```

| 옵션 | 뜻 |
|---|---|
| `-q` | 정상 요청 로그 끄기 |
| `-r dir` | 정적 파일 루트 (기본 `./www`) |
| `-c dir` | CGI 스크립트 폴더, `/cgi-bin/` 으로 접근 (기본 `./cgi-bin`, 없으면 CGI 끔) |
| `-w n` | 워커 스레드 수 (기본 8, 대기열은 n x 16) |

```sh
curl -i localhost:8080/                                   # www/index.html
curl -i 'localhost:8080/cgi-bin/hello.sh?name=me'         # CGI GET
curl -i -d 'msg=hi' localhost:8080/cgi-bin/echo.py        # CGI POST
```

## 테스트

```sh
make test        # 단위 테스트: L1 socketpair, L2 문자열, L3 응답 바이트, MIME, 스레드 풀
make e2e         # 서버를 띄워 curl/nc/python 으로 61가지 확인
make asan        # AddressSanitizer + UBSan 빌드 → make e2e
make asan-test
make tsan        # ThreadSanitizer 빌드 → make e2e
make tsan-test
```

## 제한

- `Transfer-Encoding: chunked` 요청 본문은 501
- 요청 본문 최대 1MB (413), 헤더 최대 8KB / 32개 (431)
- CGI 실행 시간 5초 (504), 출력 최대 1MB (502)
- 디렉터리 목록은 보여 주지 않음 (index.html 이 없으면 403)
