#!/usr/bin/env python3
"""MicroHTTP 기능 테스트 — 실제 ./server 프로세스를 띄우고 TCP 소켓으로 요청을 보냅니다.

잘못된 요청(깨진 헤더, 잘못된 버전 등)도 보내야 하므로 HTTP 라이브러리 대신
원시 소켓으로 바이트를 직접 보냅니다. Python 표준 라이브러리만 사용합니다.

    python3 tests/functional/test_server.py          # 전체
    python3 tests/functional/test_server.py static   # 이름에 'static'이 들어간 테스트만
"""
import os
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
PORT = 18080
IDLE_TIMEOUT = 5


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

    def __init__(self, *extra_args, port=PORT):
        self.port = port
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [SERVER, "-p", str(port), "-r", WWW, *extra_args],
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


def test_queue_full_returns_503(server):
    # 작업 스레드 1개 + 대기열 64개가 모두 찬 상태에서 새 연결은 503
    busy = Server("-t", "1", port=PORT + 1)
    held = []
    try:
        for _ in range(66):
            held.append(connect(PORT + 1))           # 아무것도 보내지 않고 붙잡아 둠
        time.sleep(0.3)
        status, headers, _ = request(b"GET / HTTP/1.1\r\nHost: a\r\n\r\n", port=PORT + 1)
        assert status == 503 and headers["connection"] == "close"
    finally:
        for sock in held:
            sock.close()
        busy.stop()


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


TESTS = [value for name, value in sorted(globals().items()) if name.startswith("test_")]


def main():
    selected = [t for t in TESTS if len(sys.argv) < 2 or sys.argv[1] in t.__name__]
    subprocess.run(["make", "-s", "server"], cwd=ROOT, check=True)
    server = Server()
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
                server = Server()
    finally:
        server.stop()
    print(f"{len(selected) - failures}/{len(selected)} functional tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
