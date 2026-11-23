# MicroHTTP

C11 + POSIX 로 처음부터 만드는 HTTP/1.1 서버. 레이어를 아래에서부터 하나씩 쌓는다.

| 레이어 | 내용 | 상태 |
|---|---|---|
| L1 | TCP 소켓 (`include/tcp.h`, `src/tcp.c`) | 완료 |
| L2 | HTTP 요청 파싱 | |
| L3 | HTTP 응답 생성 | |
| L4 | 정적 파일 서빙 | |
| L5 | 라우터 + CGI | |
| L6 | 동시성 (스레드 풀) | |

지금은 L1 위에 임시 연결 처리(`src/conn.c`)를 얹어, 빈 줄까지 받으면 고정 200 응답을 보낸다.

## 빌드와 실행

```sh
make            # ./microhttp
./microhttp     # 0.0.0.0:8080, -q 로 요청 로그 끄기, 포트는 인자로
curl -i localhost:8080/
```

## 테스트

```sh
make test       # L1 단위 테스트 (socketpair)
make e2e        # 서버를 띄워 curl/nc 로 200/400/431, 느린 클라이언트 끊기 확인
make asan       # AddressSanitizer + UBSan 빌드 (이후 make e2e 로 확인)
make asan-test  # sanitizer 를 켜고 단위 테스트
```
