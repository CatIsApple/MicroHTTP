# MicroHTTP — C로 만드는 학습용 웹 서버

외부 HTTP 라이브러리 없이 POSIX 소켓 API만으로 만든 HTTP/1.1 웹 서버입니다.
구현 범위는 [REQUIREMENTS.md](REQUIREMENTS.md), 작업 원칙은 [CLAUDE.md](CLAUDE.md)를 따릅니다.

## 빌드와 실행

```sh
make
./server -p 8080 -r ./www
```

브라우저에서 <http://127.0.0.1:8080/>을 엽니다. 종료는 Ctrl+C입니다.

| 옵션 | 의미 | 기본값 |
|---|---|---|
| `-p port` | 리스닝 포트 | 8080 |
| `-r root` | 정적 파일 루트 디렉터리 | `./www` |
| `-t threads` | 작업 스레드 수 (스레드 풀) | 8 |
| `-b address` | 바인딩할 IPv4 주소 | `127.0.0.1` |
| `-v` | 파서/수신 상세 로그 | 끔 |

## 테스트

```sh
make test   # 단위 테스트(tests/unit) + 기능 테스트(tests/functional)
make asan   # AddressSanitizer/UBSan 빌드로 전체 테스트
```

## 소스 구조

| 파일 | 역할 |
|---|---|
| `src/server.c` | main, 리스너, 스레드 풀(작업 큐), 시그널 셧다운, 연결 처리(keep-alive), 요청 처리 |
| `src/http.c` | HTTP 요청 파서, 응답 헤더 생성 (소켓 I/O 없음) |
| `src/static_file.c` | 루트 안에서 파일 찾기(403/404), MIME 타입 매핑 |
| `src/net.c` | `send_all`, 소켓 타임아웃, 오류 응답 송신 |
