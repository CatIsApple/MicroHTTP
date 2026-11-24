# MicroHTTP

C11 + POSIX 로 처음부터 만드는 HTTP/1.1 서버. 레이어를 아래에서부터 하나씩 쌓는다.

| 레이어 | 내용 | 상태 |
|---|---|---|
| L1 | TCP 소켓 (`include/tcp.h`, `src/tcp.c`) | 완료 |
| L2 | HTTP 요청 파싱 (`include/http_request.h`, `src/http_request.c`) | 완료 |
| L3 | HTTP 응답 생성 | |
| L4 | 정적 파일 서빙 | |
| L5 | 라우터 + CGI | |
| L6 | 동시성 (스레드 풀) | |

`src/conn.c` 가 L1 으로 받은 바이트를 L2 파서에 넘긴다. L3 전까지 응답은 고정 문자열이다
(정상 요청 200, 파싱 오류는 400 / 413 / 431 / 501 / 505).

## 빌드와 실행

```sh
make            # ./microhttp
./microhttp     # 0.0.0.0:8080, -q 로 요청 로그 끄기, 포트는 인자로
curl -i localhost:8080/
```

## 테스트

```sh
make test       # 단위 테스트 (L1: socketpair, L2: 문자열 입력)
make e2e        # 서버를 띄워 curl/nc 로 상태 코드, POST 본문, 느린 클라이언트 끊기 확인
make asan       # AddressSanitizer + UBSan 빌드 (이후 make e2e 로 확인)
make asan-test  # sanitizer 를 켜고 단위 테스트
```
