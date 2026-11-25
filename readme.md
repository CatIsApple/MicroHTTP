# MicroHTTP — C로 만드는 학습용 웹 서버

## 기본 과제 달성 현황 (STEP 0 베이스라인, 2026-11-25)

인증 기능을 넣기 **전** 상태입니다. 베이스라인에서 발견한 TSan 레이스 1건은 STEP 1 전에 고쳤고, 표는 수정 후 상태입니다(성능은 수정 전후 차이 없음). 이후 모든 비교와 회귀 검증의 기준점입니다. 측정 방법과 원시 결과는 [EXPERIMENT_REPORT.md](EXPERIMENT_REPORT.md) STEP 0에 있습니다.

| 영역 | LMS 체크리스트 항목 | 상태 | 근거 (코드 / 테스트) |
|---|---|---|---|
| 소켓 | `socket → bind → listen → accept`, `SO_REUSEADDR`, bind 실패 처리 | ✅ | `server.c` `create_listener`, `main` |
| 소켓 | 블로킹 + 타임아웃(클라이언트), 논블로킹 connect + poll(백엔드) | ✅ | `net.c` `set_socket_timeout`, `connect_with_timeout` |
| HTTP | 요청 줄·헤더·본문 경계, `Content-Length`, GET/POST | ✅ | `http.c` `http_parse_request`, `test_http.c` |
| HTTP | keep-alive, 파이프라이닝, 연결당 100요청, 유휴 5초 | ✅ | `server.c` `handle_connection`, 기능 테스트 4개 |
| HTTP | 정적 파일, MIME, 404/403/500, 경로 탈출 방지 | ✅ | `static_file.c`, `test_static.c`, `test_mime.c` |
| HTTP | 오류 응답 400/408/411/413/414/417/431/501/505 | ✅ | `test_http.c`, `edge_probe.py` 30케이스 |
| 동시성 | 스레드 풀(기본 8) + 작업 큐(64) + 배압 | ✅ | `server.c` `queue_push`/`queue_pop`/`worker_main` |
| 동시성 | SIGINT/SIGTERM 안전한 셧다운, SIGPIPE 무시 | ✅ | `test_graceful_shutdown_…`. 베이스라인에서 TSan이 `stop_requested` 데이터 레이스를 보고 → C11 `atomic_int`로 수정 (EXPERIMENT_REPORT STEP 0-1) |
| Proxy | Reverse Proxy, 경로 라우팅(최장 prefix), Round-Robin | ✅ | `proxy.c`, `test_proxy.c` |
| Proxy | 502(연결 불가)/504(타임아웃), 장애 백엔드 건너뛰기, 떠난 클라이언트 감지 | ✅ | 기능 테스트 10개 |
| Proxy | Forward Proxy | ➖ | 요구사항은 "최소 한 가지" — Reverse Proxy 선택 |
| 메모리·안정성 | ASan + UBSan | ✅ | `make asan` 28/28 |
| 메모리·안정성 | ThreadSanitizer | ✅ | `make tsan` 28/28 (macOS clang, Linux gcc), TSan 빌드 서버 엣지 탐침 30/30 · 보고 0. 베이스라인은 27/28 |
| 메모리·안정성 | Valgrind(누수·잘못된 접근), 일반 사용자 테스트, malloc 실패 → 500 (Linux) | ✅ | `tests/linux/verify.sh` → `ALL LINUX CHECKS PASSED` |
| 메모리·안정성 | fd 누수 | ✅ | 부하 측정 전후 fd 7 → 7, Valgrind `--track-fds` 종료 시 stdio + 로그만 열림 |
| 메모리·안정성 | 엣지 케이스(불완전 헤더, Slowloris, 큰 본문, 긴 URI, 백엔드 타임아웃, 갑작스러운 종료) | ✅ | `edge_probe.py` 30/30, 기능 테스트 28/28 |

**베이스라인 성능** (hey, 3회 중앙값, M1 Pro 루프백, 작업 스레드 8)

| 시나리오 | 요청/초 | p95 ms | p99 ms | 비정상 응답 |
|---|---:|---:|---:|---:|
| 정적 `GET /`, keep-alive, 동시 8 | 37,700 | 0.3 | 0.6 | 0 |
| 정적, keep-alive, 동시 50 | 39,471 | 0.3 | 4.7 | 0 |
| 정적, 요청마다 새 연결, 동시 10 | 18,248 | 0.9 | 1.2 | 0 |
| 정적, 요청마다 새 연결, 동시 50 | 19,096 | 4.4 | 5.3 | 0 |
| 프록시 `GET /api/x`, 동시 10 | 5,365 | 2.2 | 2.9 | 0 |


외부 HTTP 라이브러리 없이 POSIX 소켓 API만으로 만든 HTTP/1.1 웹 서버입니다.
구현 범위는 [REQUIREMENTS.md](REQUIREMENTS.md), 작업 원칙은 [CLAUDE.md](CLAUDE.md)를 따릅니다.

| 문서 | 내용 |
|---|---|
| [CODE_FLOW.md](CODE_FLOW.md) | 서버 실행부터 종료까지, 연결부터 응답까지의 함수 호출 순서 |
| [STUDY_GUIDE.md](STUDY_GUIDE.md) | 핵심 개념 12개를 What/Why/How/Without/Code로 정리 + 확인 질문 |
| [IMPLEMENTATION_REPORT.md](IMPLEMENTATION_REPORT.md) | 요구사항별 구현 상태, 테스트 명령어와 실제 결과, 한계 |
| [QUALITY_REPORT.md](QUALITY_REPORT.md) | 품질 기준(단위·기능·부하·메모리·엣지 케이스)별 검증 결과와 발견·수정한 결함 |
| [EXPERIMENT_REPORT.md](EXPERIMENT_REPORT.md) | 인증 과제 실험 기록 (STEP 0 베이스라인부터) |

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
