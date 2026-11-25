# MicroHTTP — C로 만드는 학습용 웹 서버

외부 HTTP 라이브러리 없이 POSIX 소켓 API만으로 만든 HTTP/1.1 웹 서버입니다.
구현 범위는 [REQUIREMENTS.md](REQUIREMENTS.md), 작업 원칙은 [CLAUDE.md](CLAUDE.md)를 따릅니다.

| 문서 | 내용 |
|---|---|
| [CODE_FLOW.md](CODE_FLOW.md) | 서버 실행부터 종료까지, 연결부터 응답까지의 함수 호출 순서 |
| [STUDY_GUIDE.md](STUDY_GUIDE.md) | 핵심 개념 12개를 What/Why/How/Without/Code로 정리 + 확인 질문 |
| [IMPLEMENTATION_REPORT.md](IMPLEMENTATION_REPORT.md) | 요구사항별 구현 상태, 테스트 명령어와 실제 결과, 한계 |
| [QUALITY_REPORT.md](QUALITY_REPORT.md) | 품질 기준(단위·기능·부하·메모리·엣지 케이스)별 검증 결과와 발견·수정한 결함 |

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
| `-R prefix=host:port[,host:port...]` | Reverse Proxy 규칙 (여러 번 지정 가능) | 없음 |
| `-b address` | 바인딩할 IPv4 주소 | `127.0.0.1` |
| `-v` | 파서/수신 상세 로그 | 끔 |

## Reverse Proxy 실습

```sh
python3 tools/backend.py 9001 A &     # 실습용 백엔드 (Python 표준 라이브러리)
python3 tools/backend.py 9002 B &
./server -R /api/=127.0.0.1:9001,127.0.0.1:9002
curl http://127.0.0.1:8080/api/hello  # A, B가 번갈아 응답 (Round-Robin)
```

`/api/`로 시작하는 요청은 백엔드로 전달하고, 나머지는 정적 파일로 응답합니다.
연결할 수 없는 백엔드는 건너뛰며, 모두 실패하면 502, 응답이 5초 동안 없으면 504입니다.

## 테스트

```sh
make test   # 단위 테스트(tests/unit) + 기능 테스트(tests/functional)
make asan   # AddressSanitizer/UBSan 빌드로 전체 테스트
sh tests/functional/curl_check.sh   # curl로 정적 파일·프록시 확인
python3 tests/functional/edge_probe.py <port>   # 떠 있는 서버에 엣지 케이스 30개 (사용법은 파일 머리말)
docker build -t microhttp-verify tests/linux && docker run --rm -v "$PWD":/src:ro microhttp-verify sh /src/tests/linux/verify.sh   # Linux: 일반 사용자 테스트, Valgrind, malloc 실패 주입
```

## 소스 구조

| 파일 | 역할 |
|---|---|
| `src/server.c` | main, 리스너, 스레드 풀(작업 큐), 시그널 셧다운, 연결 처리(keep-alive), 요청 처리 |
| `src/http.c` | HTTP 요청 파서, 응답 헤더 생성 (소켓 I/O 없음) |
| `src/static_file.c` | 루트 안에서 파일 찾기(403/404), MIME 타입 매핑 |
| `src/proxy.c` | Reverse Proxy: 경로 라우팅, Round-Robin, 요청 재작성, 응답 중계, 502/504 |
| `src/net.c` | `send_all`, 소켓 타임아웃, 타임아웃 있는 connect, 오류 응답 송신 |
| `tools/backend.py` | 프록시 실습용 백엔드 (테스트 도구) |
