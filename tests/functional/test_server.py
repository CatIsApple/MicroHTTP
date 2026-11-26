#!/usr/bin/env python3
"""MicroHTTP 기능 테스트 — 실제 ./server 프로세스를 띄우고 TCP 소켓으로 요청을 보냅니다.

잘못된 요청(깨진 헤더, 잘못된 버전 등)도 보내야 하므로 HTTP 라이브러리 대신
원시 소켓으로 바이트를 직접 보냅니다. Python 표준 라이브러리만 사용합니다.

    python3 tests/functional/test_server.py          # 전체
    python3 tests/functional/test_server.py static   # 이름에 'static'이 들어간 테스트만
"""
import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SERVER = os.path.join(ROOT, "server")
WWW = os.path.join(ROOT, "www")
USERS = os.path.join(ROOT, "tests", "functional", "users.txt")   # userA / userA-test-pw, userB / userB-test-pw
PORT = 18080
IDLE_TIMEOUT = 5
REQUEST_TIMEOUT = 10
BACKEND_TIMEOUT = 5
BACKEND_A, BACKEND_B, BACKEND_DEAD = 19001, 19002, 19009
PROXY_ARGS = [
    "-R", f"/api/=127.0.0.1:{BACKEND_A},127.0.0.1:{BACKEND_B}",          # Round-Robin
    "-R", f"/down/=127.0.0.1:{BACKEND_DEAD}",                            # 아무도 듣지 않는 포트
    "-R", f"/failover/=127.0.0.1:{BACKEND_DEAD},127.0.0.1:{BACKEND_A}",  # 죽은 백엔드 건너뛰기
]


# ── 서버 실행 도우미 ──────────────────────────────────────────────────────

def wait_for_port(port, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"port {port} did not open")


class Server:
    """./server를 실행하고 stderr 로그를 임시 파일에 모읍니다."""

    def __init__(self, *extra_args, port=PORT, root=WWW):
        self.port = port
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [SERVER, "-p", str(port), "-r", root, *extra_args],
            stdout=subprocess.DEVNULL, stderr=self.log)
        wait_for_port(port)

    def logs(self):
        self.log.seek(0)
        return self.log.read().decode(errors="replace")

    def alive(self):
        return self.process.poll() is None

    def stop(self):
        if self.alive():
            self.process.kill()
            self.process.wait()
        self.log.close()


def start_backend(port, name):
    process = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "backend.py"), str(port), name],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_for_port(port)
    return process


# ── 원시 HTTP 도우미 ──────────────────────────────────────────────────────

class Conn:
    """소켓 + 읽기 버퍼. 응답 하나를 읽고 남은 바이트(파이프라이닝된 다음 응답)를 보관합니다."""

    def __init__(self, sock):
        self.sock = sock
        self.buffer = b""

    def sendall(self, data):
        self.sock.sendall(data)

    def recv_more(self):
        chunk = self.sock.recv(65536)
        self.buffer += chunk
        return chunk

    def __getattr__(self, name):          # settimeout, shutdown, setsockopt, close ...
        return getattr(self.sock, name)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.sock.close()


def connect(port=PORT, timeout=10):
    return Conn(socket.create_connection(("127.0.0.1", port), timeout=timeout))


def read_response(conn, head=False):
    """응답 하나를 읽어 (status, headers dict, body bytes)를 돌려줍니다.
    Content-Length가 있으면 그만큼, 없으면 연결이 닫힐 때까지 본문을 읽습니다."""
    while b"\r\n\r\n" not in conn.buffer:
        if not conn.recv_more():
            raise ConnectionError(f"connection closed before headers: {conn.buffer!r}")
    head_bytes, conn.buffer = conn.buffer.split(b"\r\n\r\n", 1)
    lines = head_bytes.decode("latin-1").split("\r\n")
    status = int(lines[0].split(" ")[1])
    headers = {}
    for line in lines[1:]:
        name, value = line.split(":", 1)
        headers[name.strip().lower()] = value.strip()
    if head:
        return status, headers, b""
    if "content-length" in headers:
        length = int(headers["content-length"])
        while len(conn.buffer) < length and conn.recv_more():
            pass
        body, conn.buffer = conn.buffer[:length], conn.buffer[length:]
        return status, headers, body
    while conn.recv_more():
        pass
    body, conn.buffer = conn.buffer, b""
    return status, headers, body


def request(raw, port=PORT, head=False):
    """요청 바이트를 보내고 응답 하나를 읽습니다 (새 연결)."""
    with connect(port) as sock:
        sock.sendall(raw)
        return read_response(sock, head=head)


def get(path, port=PORT, extra=b""):
    return request(b"GET " + path.encode() + b" HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + extra + b"\r\n", port)


def is_closed(sock, wait=2.0):
    """서버가 연결을 닫았으면 True."""
    sock.settimeout(wait)
    try:
        return sock.recv_more() == b""
    except socket.timeout:
        return False
    except ConnectionResetError:
        return True


def file_bytes(relative):
    with open(os.path.join(WWW, relative), "rb") as f:
        return f.read()


# ── 정적 파일 / MIME ─────────────────────────────────────────────────────

def test_static_files_and_mime(server):
    cases = [
        ("/", "index.html", "text/html; charset=utf-8"),
        ("/index.html", "index.html", "text/html; charset=utf-8"),
        ("/css/style.css", "css/style.css", "text/css; charset=utf-8"),
        ("/js/app.js", "js/app.js", "text/javascript; charset=utf-8"),
        ("/images/logo.png", "images/logo.png", "image/png"),
        ("/images/photo.jpg", "images/photo.jpg", "image/jpeg"),
    ]
    for path, relative, mime in cases:
        status, headers, body = get(path)
        expected = file_bytes(relative)
        assert status == 200, (path, status)
        assert headers["content-type"] == mime, (path, headers)
        assert int(headers["content-length"]) == len(expected)
        assert body == expected, path


def test_static_head_has_no_body(server):
    with connect() as sock:
        sock.sendall(b"HEAD /index.html HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n")
        status, headers, _ = read_response(sock, head=True)
        assert status == 200 and int(headers["content-length"]) == len(file_bytes("index.html"))
        assert is_closed(sock) and sock.buffer == b""     # 본문 없이 닫힘


def test_static_errors_404_403_405(server):
    assert get("/missing.html")[0] == 404
    assert get("/../readme.md")[0] == 403              # 루트 탈출
    assert get("/css/../../Makefile")[0] == 403
    assert get("/%2e%2e/Makefile")[0] == 403
    assert get("/bad%zz")[0] == 400
    status, headers, _ = request(b"POST /index.html HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi")
    assert status == 405 and headers["allow"] == "GET, HEAD"
    status, _, _ = request(b"DELETE / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n")
    assert status == 405


# ── keep-alive / Connection ──────────────────────────────────────────────

def test_keepalive_reuses_connection(server):
    with connect() as sock:
        for path in ("/", "/css/style.css", "/js/app.js"):
            sock.sendall(f"GET {path} HTTP/1.1\r\nHost: a\r\n\r\n".encode())
            status, headers, _ = read_response(sock)
            assert status == 200 and headers["connection"] == "keep-alive"
        assert not is_closed(sock, wait=0.5)          # 아직 열려 있음


def test_connection_close_and_http10(server):
    with connect() as sock:
        sock.sendall(b"GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n")
        status, headers, _ = read_response(sock)
        assert status == 200 and headers["connection"] == "close" and is_closed(sock)
    with connect() as sock:                           # HTTP/1.0 기본은 close, Host 없어도 됨
        sock.sendall(b"GET / HTTP/1.0\r\n\r\n")
        status, headers, _ = read_response(sock)
        assert status == 200 and headers["connection"] == "close" and is_closed(sock)
    with connect() as sock:                           # HTTP/1.0 + keep-alive
        sock.sendall(b"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
        status, headers, _ = read_response(sock)
        assert headers["connection"] == "keep-alive" and not is_closed(sock, wait=0.5)


def test_pipelined_requests(server):
    with connect() as sock:
        sock.sendall(b"GET /css/style.css HTTP/1.1\r\nHost: a\r\n\r\n"
                     b"GET /js/app.js HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n")
        first = read_response(sock)
        assert first[0] == 200 and first[1]["content-type"].startswith("text/css")
        second = read_response(sock)
        assert second[0] == 200 and second[1]["content-type"].startswith("text/javascript")


def test_keepalive_idle_timeout_closes_quietly(server):
    with connect() as sock:
        sock.sendall(b"GET / HTTP/1.1\r\nHost: a\r\n\r\n")
        read_response(sock)
        start = time.time()
        assert is_closed(sock, wait=IDLE_TIMEOUT + 3)  # 다음 요청이 없으면 서버가 닫음 (408 없이)
        assert IDLE_TIMEOUT - 1 <= time.time() - start <= IDLE_TIMEOUT + 2


# ── 비정상 요청 ─────────────────────────────────────────────────────────

def test_malformed_requests(server):
    cases = [
        (b"GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505),           # 잘못된 버전
        (b"GET / HTTX/1.1\r\nHost: a\r\n\r\n", 400),
        (b"GE(T / HTTP/1.1\r\nHost: a\r\n\r\n", 400),          # 비정상 메서드
        (b"GET / HTTP/1.1\r\n\r\n", 400),                      # Host 없음
        (b"GET / HTTP/1.1\r\nBroken header\r\n\r\n", 400),     # 콜론 없는 헤더
        (b"POST / HTTP/1.1\r\nHost: a\r\n\r\n", 411),          # 길이 없는 POST
        (b"POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 501),
    ]
    for raw, expected in cases:
        status, headers, _ = request(raw)
        assert status == expected, (raw, status)
        assert headers["connection"] == "close"


def test_incomplete_header_then_close(server):
    with connect() as sock:
        sock.sendall(b"GET / HTTP/1.1\r\nHost: a\r\n")   # 빈 줄 없이
        sock.shutdown(socket.SHUT_WR)                    # 클라이언트가 더 보내지 않음(EOF)
        assert read_response(sock)[0] == 400


def test_incomplete_header_timeout_408(server):
    with connect(timeout=IDLE_TIMEOUT + 5) as sock:
        start = time.time()
        sock.sendall(b"GET / HTTP/1.1\r\nHost:")
        assert read_response(sock)[0] == 408
        assert time.time() - start >= IDLE_TIMEOUT - 1


def test_server_side_errors_500_and_file_shrinks_midway(server):
    """서버 쪽 문제: 링크 순환 → 500, 보내는 도중 파일이 줄어듦 → 200 헤더 뒤라 500으로 못 바꾸니 연결을 끊음."""
    big_size = 32 * 1024 * 1024
    with tempfile.TemporaryDirectory() as root:
        os.symlink("loop_b", os.path.join(root, "loop_a"))
        os.symlink("loop_a", os.path.join(root, "loop_b"))
        big = os.path.join(root, "big.bin")
        with open(big, "wb") as f:
            f.truncate(big_size)
        with open(os.path.join(root, "ok.txt"), "w") as f:
            f.write("ok")
        other = Server(port=PORT + 7, root=root)
        try:
            # 1) 500: 요청은 정상인데 서버의 파일 시스템 설정이 잘못됨 (keep-alive는 유지)
            with connect(PORT + 7) as sock:
                sock.sendall(b"GET /loop_a HTTP/1.1\r\nHost: a\r\n\r\n")
                status, headers, _ = read_response(sock)
                assert status == 500 and headers["connection"] == "keep-alive"
                sock.sendall(b"GET /" + b"n" * 300 + b" HTTP/1.1\r\nHost: a\r\n\r\n")
                assert read_response(sock)[0] == 414          # 이름이 너무 김 = 클라이언트 쪽 문제

            # 2) 헤더(200, Content-Length 32MB)를 받은 뒤 파일을 0바이트로 자름
            with connect(PORT + 7, timeout=15) as sock:
                sock.sendall(b"GET /big.bin HTTP/1.1\r\nHost: a\r\n\r\n")
                status, headers, _ = read_response(sock, head=True)
                assert status == 200 and int(headers["content-length"]) == big_size
                time.sleep(0.3)                     # 서버가 소켓 버퍼를 채우고 send에서 멈출 때까지
                os.truncate(big, 0)                 # 이후 read()는 0(EOF)을 돌려줌
                received = len(sock.buffer)
                while True:
                    chunk = sock.recv(65536)
                    if not chunk:
                        break
                    received += len(chunk)
                assert received < big_size, received    # 덜 받고 연결이 끊김 → 클라이언트가 실패를 알 수 있음
            assert get("/ok.txt", port=PORT + 7)[2] == b"ok"   # 서버는 계속 동작
            assert other.alive()
        finally:
            other.stop()


def test_slow_request_deadline_408(server):
    """slowloris: 1초마다 1바이트씩 보내면 recv 대기(5초)는 매번 다시 시작되지만, 요청 총 한도(10초)에서 끊긴다."""
    request = b"GET / HTTP/1.1\r\nHost: a\r\nX-Pad: " + b"p" * 64 + b"\r\n\r\n"  # 다 보내려면 100초 이상
    with connect(timeout=REQUEST_TIMEOUT + IDLE_TIMEOUT + 5) as sock:
        start = time.time()
        for byte in request:
            sock.sendall(bytes([byte]))
            readable, _, _ = select.select([sock], [], [], 1.0)  # 1초 쉬되, 응답이 오면 바로 멈춤
            if readable:
                break
        assert read_response(sock)[0] == 408
        elapsed = time.time() - start
        assert REQUEST_TIMEOUT - 1 <= elapsed <= REQUEST_TIMEOUT + 3, elapsed


def test_large_body_and_long_uri(server):
    status, _, _ = request(b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 999999999\r\n\r\n")
    assert status == 413
    assert get("/" + "a" * 3000)[0] == 414
    status, _, _ = request(b"GET / HTTP/1.1\r\nHost: a\r\nX-Big: " + b"b" * 20000 + b"\r\n\r\n")
    assert status == 431


def test_sudden_client_disconnect(server):
    # 1) 헤더 도중 끊기  2) RST로 끊기  3) 응답을 읽지 않고 끊기 → 서버는 계속 살아 있어야 함
    with connect() as sock:
        sock.sendall(b"GET / HTT")
    sock = connect()
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    sock.sendall(b"GET /images/logo.png HTTP/1.1\r\nHost: a\r\n\r\n")
    sock.close()                                      # linger 0 → RST
    for _ in range(20):
        with connect() as sock:
            sock.sendall(b"GET /index.html HTTP/1.1\r\nHost: a\r\n\r\n")
    time.sleep(0.3)
    assert server.alive()
    assert get("/")[0] == 200


# ── 동시 처리 (스레드 풀) ─────────────────────────────────────────────────

def test_concurrent_slow_client_does_not_block_others(server):
    # 느린 클라이언트가 작업 스레드 하나를 붙잡고 있어도 다른 요청은 바로 처리되어야 함
    slow = connect()
    slow.sendall(b"GET / HTTP/1.1\r\nHost:")         # 헤더를 보내다 멈춤
    time.sleep(0.2)
    start = time.time()
    assert get("/index.html")[0] == 200
    assert time.time() - start < 1.0, "blocked behind slow client"
    slow.close()


def test_concurrent_many_clients(server):
    import concurrent.futures
    expected = file_bytes("css/style.css")

    def client(_):
        results = []
        with connect() as sock:                       # 연결 하나로 keep-alive 10회
            for _ in range(10):
                sock.sendall(b"GET /css/style.css HTTP/1.1\r\nHost: a\r\n\r\n")
                status, _, body = read_response(sock)
                results.append(status == 200 and body == expected)
        return all(results)

    with concurrent.futures.ThreadPoolExecutor(max_workers=50) as pool:
        assert all(pool.map(client, range(200)))     # 200 연결 × 10 요청 = 2000 요청


def test_pool_exhausted_requests_wait_not_fail(server):
    # 작업 스레드가 1개뿐이고 그 스레드를 느린 클라이언트가 붙잡으면, 다음 요청은 실패하지 않고
    # 큐에서 기다렸다가 스레드가 풀려난 뒤(유휴 타임아웃 5초) 처리됩니다. → 스레드 풀 크기의 한계
    single = Server("-t", "1", port=PORT + 1)
    try:
        slow = connect(PORT + 1)
        slow.sendall(b"GET / HTTP/1.1\r\nHost:")              # 유일한 작업 스레드를 점유
        time.sleep(0.2)
        start = time.time()
        status, _, _ = request(b"GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", port=PORT + 1)
        waited = time.time() - start
        assert status == 200
        assert IDLE_TIMEOUT - 1 <= waited <= IDLE_TIMEOUT + 2, waited
        assert read_response(slow)[0] == 408                    # 느린 쪽은 타임아웃
        slow.close()
    finally:
        single.stop()


# ── 시그널 / 셧다운 ──────────────────────────────────────────────────────

def test_graceful_shutdown_finishes_inflight_request(server):
    import signal
    for signal_number in (signal.SIGTERM, signal.SIGINT):
        target = Server(port=PORT + 2)
        try:
            sock = connect(PORT + 2)
            sock.sendall(b"GET /index.html HTTP/1.1\r\nHost: a\r\n")   # 요청을 보내는 도중
            time.sleep(0.2)
            target.process.send_signal(signal_number)                    # 셧다운 시작
            time.sleep(0.3)
            sock.sendall(b"\r\n")                                       # 요청 마저 보내기
            status, headers, body = read_response(sock)
            assert status == 200 and body == file_bytes("index.html")
            assert headers["connection"] == "close"                     # 마지막 응답
            assert target.process.wait(timeout=IDLE_TIMEOUT + 3) == 0   # 정상 종료 코드
            assert "shutdown complete" in target.logs()
            try:
                connect(PORT + 2, timeout=1).close()
                raise AssertionError("listener still open")
            except ConnectionRefusedError:
                pass
            sock.close()
        finally:
            target.stop()


# ── Reverse Proxy ───────────────────────────────────────────────────────

def proxy_get(path, extra=b""):
    import json
    status, headers, body = request(b"GET " + path.encode() + b" HTTP/1.1\r\nHost: front.test\r\n" + extra + b"\r\n")
    return status, headers, (json.loads(body) if status == 200 else body)


def test_proxy_routes_api_to_backend_and_static_locally(server):
    status, headers, data = proxy_get("/api/hello?x=1")
    assert status == 200 and headers["content-type"].startswith("application/json")
    assert data["path"] == "/api/hello?x=1" and data["method"] == "GET"
    assert get("/index.html")[0] == 200                      # 규칙에 없는 경로는 정적 파일


def test_proxy_round_robin(server):
    names = [proxy_get("/api/rr")[2]["backend"] for _ in range(6)]
    assert set(names) == {"A", "B"}, names
    assert all(names[i] != names[i + 1] for i in range(5)), names   # 번갈아 가며


def test_proxy_forwards_post_body_and_headers(server):
    import json
    body = "안녕 proxy".encode() + bytes(range(1, 32))
    raw = (b"POST /api/echo HTTP/1.1\r\nHost: front.test\r\nContent-Type: text/plain\r\n"
           b"X-Custom: yes\r\nConnection: keep-alive\r\nKeep-Alive: timeout=5\r\n"
           b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
    with connect() as sock:
        sock.sendall(raw)
        status, headers, payload = read_response(sock)
        data = json.loads(payload)
        assert status == 200 and data["method"] == "POST"
        assert data["body_bytes"] == len(body)
        seen = data["headers"]
        assert seen["host"] == "front.test" and seen["x-custom"] == "yes"   # 원래 헤더 유지
        assert seen["connection"] == "close" and "keep-alive" not in seen   # hop-by-hop 교체
        assert is_closed(sock)                                              # 프록시 응답 후 연결 닫힘


def test_proxy_http10_without_host(server):
    import json
    status, _, body = request(b"GET /api/old HTTP/1.0\r\n\r\n")
    assert status == 200 and json.loads(body)["headers"]["host"].startswith("127.0.0.1:")


def test_proxy_backend_down_502(server):
    status, headers, _ = proxy_get("/down/x")
    assert status == 502 and headers["connection"] == "close"


def test_proxy_failover_skips_dead_backend(server):
    for _ in range(4):
        status, _, data = proxy_get("/failover/x")
        assert status == 200 and data["backend"] == "A"


def test_proxy_backend_timeout_504(server):
    start = time.time()
    status, _, _ = proxy_get("/api/slow")
    elapsed = time.time() - start
    assert status == 504, status
    assert BACKEND_TIMEOUT - 1 <= elapsed <= BACKEND_TIMEOUT + 2, elapsed


def test_proxy_client_disconnects_midway(server):
    sock = connect()
    sock.sendall(b"GET /api/slow HTTP/1.1\r\nHost: a\r\n\r\n")
    sock.close()                                       # 백엔드 응답 전에 클라이언트가 떠남
    assert proxy_get("/api/after")[0] == 200
    assert server.alive()


def test_proxy_client_closes_after_full_response_is_not_an_abort(server):
    """응답을 다 받은 클라이언트가 백엔드보다 먼저 연결을 닫는 것은 정상이다(상태 200으로 기록)."""
    before = server.logs().count("client left while waiting")
    with connect() as sock:
        sock.sendall(b"GET /api/linger HTTP/1.1\r\nHost: a\r\n\r\n")
        status, headers, body = read_response(sock)        # Content-Length만큼 다 읽음
        assert status == 200 and b"/api/linger" in body
    time.sleep(1.5)                                        # 백엔드가 늦게 닫을 때까지
    logs = server.logs()
    assert logs.count("client left while waiting") == before
    assert '"GET /api/linger HTTP/1.1" status=200' in logs


def test_proxy_frees_worker_when_client_leaves(server):
    """느린 백엔드를 기다리던 클라이언트들이 떠나면 작업 스레드가 바로 풀려야 한다.
    스레드(8)의 두 배인 16개를 보내고 모두 끊는다. 감지하지 못하면 스레드가 백엔드 타임아웃(5초)씩
    두 차례 묶여 새 요청이 최대 10초를 기다린다."""
    abandoned = [connect() for _ in range(16)]
    for sock in abandoned:
        sock.sendall(b"GET /api/slow HTTP/1.1\r\nHost: a\r\n\r\n")
    time.sleep(0.5)                        # 작업 스레드들이 요청을 읽고 백엔드를 기다리기 시작할 때까지
    for sock in abandoned:
        sock.close()                       # FIN: 요청은 이미 도착했고, 이제 응답을 받을 사람이 없음
    start = time.time()
    assert get("/index.html")[0] == 200
    elapsed = time.time() - start
    assert elapsed < 2.0, f"worker threads stayed busy for {elapsed:.1f}s"
    time.sleep(0.5)
    assert "client left while waiting" in server.logs()


# ── 인증: Cookie + 서버 측 Session (STEP 1) ───────────────────────────────

import re

SID = re.compile(r"sid=([0-9a-f]{64})")


def login(user, password, cookie=None, sock=None):
    """POST /login. (status, headers, 새 sid 또는 None)"""
    body = f"username={user}&password={password}".encode()
    raw = (b"POST /login HTTP/1.1\r\nHost: a\r\nContent-Type: application/x-www-form-urlencoded\r\n"
           + (f"Cookie: sid={cookie}\r\n".encode() if cookie else b"")
           + (b"" if sock else b"Connection: close\r\n")
           + f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    if sock:
        sock.sendall(raw)
        status, headers, _ = read_response(sock)
    else:
        status, headers, _ = request(raw)
    match = SID.search(headers.get("set-cookie", ""))
    return status, headers, match.group(1) if match else None


def me(cookie_header=None, sock=None):
    raw = (b"GET /me HTTP/1.1\r\nHost: a\r\n" + (f"Cookie: {cookie_header}\r\n".encode() if cookie_header else b"")
           + (b"" if sock else b"Connection: close\r\n") + b"\r\n")
    if sock:
        sock.sendall(raw)
        return read_response(sock)
    return request(raw)


def test_auth_login_cookie_attributes_and_new_connections(server):
    status, headers, sid = login("userA", "userA-test-pw")
    assert status == 303 and headers["location"] == "/me" and sid
    cookie = headers["set-cookie"]
    for attribute in ("HttpOnly", "SameSite=Lax", "Path=/"):
        assert attribute in cookie, cookie
    assert headers["cache-control"] == "no-store"
    # 로그인한 연결은 이미 닫혔다. 새 TCP 연결 3개에서 같은 Cookie로 인증이 유지되어야 한다.
    for _ in range(3):
        status, headers, body = me(f"theme=dark; sid={sid}")
        assert status == 200 and body == b"user: userA\n" and headers["cache-control"] == "no-store"
    assert sid not in server.logs()                     # Session ID를 로그에 남기지 않음


def test_auth_rejects_missing_wrong_and_forged_credentials(server):
    assert me()[0] == 401                                                # Cookie 없음
    assert login("userA", "wrong")[0] == 401                             # 틀린 비밀번호
    assert login("mallory", "userA-test-pw")[0] == 401                   # 없는 사용자
    assert login("userA", "")[0] == 401
    _, _, sid = login("userA", "userA-test-pw")
    tampered = sid[:-1] + ("0" if sid[-1] != "0" else "1")
    assert me(f"sid={tampered}")[0] == 401                               # 한 글자 바꾼 ID
    assert me("sid=" + "a" * 64)[0] == 401                               # 형식만 맞춘 위조 ID
    assert me("user=userA; logged_in=true; role=admin")[0] == 401        # 상태를 담은 위조 Cookie
    assert me("sid=userA")[0] == 401                                     # 사용자 이름을 ID로
    assert me(f"sid={sid}")[0] == 200                                    # 원래 ID는 여전히 유효


def test_auth_logout_invalidates_on_server(server):
    _, _, sid = login("userB", "userB-test-pw")
    assert me(f"sid={sid}")[0] == 200
    raw = f"POST /logout HTTP/1.1\r\nHost: a\r\nCookie: sid={sid}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
    status, headers, _ = request(raw.encode())
    assert status == 200 and "Max-Age=0" in headers["set-cookie"]
    # 클라이언트가 지우라는 말을 무시하고 옛 Cookie를 다시 보내도 서버가 거절해야 한다.
    assert me(f"sid={sid}")[0] == 401


def test_auth_login_reissues_session_id(server):
    _, _, first = login("userA", "userA-test-pw")
    status, _, second = login("userA", "userA-test-pw", cookie=first)
    assert status == 303 and second and second != first                 # 로그인마다 새 ID
    assert me(f"sid={first}")[0] == 401                                 # 이전 ID는 폐기 (Session 고정 방지)
    assert me(f"sid={second}")[0] == 200


def test_auth_is_per_request_not_per_connection(server):
    """하나의 keep-alive 연결에서도 인증은 요청마다 Cookie로 판단한다."""
    with connect() as sock:
        assert me(sock=sock)[0] == 401
        _, _, sid = login("userA", "userA-test-pw", sock=sock)
        assert me(f"sid={sid}", sock=sock)[0] == 200
        assert me(sock=sock)[0] == 401                                   # 같은 연결이라도 Cookie가 없으면 401
        _, _, userB = login("userB", "userB-test-pw", sock=sock)
        assert me(f"sid={userB}", sock=sock)[2] == b"user: userB\n"          # 같은 연결에서 다른 사용자
        assert me(f"sid={sid}", sock=sock)[2] == b"user: userA\n"


def test_auth_methods(server):
    status, headers, _ = get("/login")
    assert status == 405 and headers["allow"] == "POST"
    raw = b"POST /me HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
    status, headers, _ = request(raw)
    assert status == 405 and headers["allow"] == "GET, HEAD"


TESTS = [value for name, value in sorted(globals().items()) if name.startswith("test_")]


def main():
    selected = [t for t in TESTS if len(sys.argv) < 2 or sys.argv[1] in t.__name__]
    subprocess.run(["make", "-s", "server"], cwd=ROOT, check=True)
    backends = [start_backend(BACKEND_A, "A"), start_backend(BACKEND_B, "B")]
    server = Server(*PROXY_ARGS, "-U", USERS)
    failures = 0
    try:
        for test in selected:
            start = time.time()
            try:
                test(server)
                print(f"PASS {test.__name__} ({time.time() - start:.2f}s)")
            except Exception:
                failures += 1
                print(f"FAIL {test.__name__}")
                traceback.print_exc()
            if not server.alive():
                print("server died; restarting")
                print(server.logs()[-3000:])
                server.stop()
                server = Server(*PROXY_ARGS, "-U", USERS)
        # 새니타이저 빌드(make asan / make tsan)에서 서버 로그에 메모리 오류, 데이터 레이스, UB 보고가 있으면 실패로 처리
        for marker in ("AddressSanitizer", "LeakSanitizer", "ThreadSanitizer", "runtime error:"):
            if marker in server.logs():
                failures += 1
                print(f"FAIL sanitizer report in server log: {marker}")
                print(server.logs()[-5000:])
    finally:
        server.stop()
        for backend in backends:
            backend.kill()
            backend.wait()
    print(f"{len(selected) - failures}/{len(selected)} functional tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
