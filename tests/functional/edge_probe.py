"""엣지 케이스 탐침 — 이미 떠 있는 서버에 비정상 요청을 보내고 결과를 표로 출력합니다.

    python3 tools/backend.py 19201 A &
    cp -r www /tmp/probe_root && dd if=/dev/urandom of=/tmp/probe_root/big.bin bs=1048576 count=64  # 큰 파일(선택)
    ./server -p 18200 -r /tmp/probe_root -R /api/=127.0.0.1:19201 -R /down/=127.0.0.1:19209 &
    python3 tests/functional/edge_probe.py 18200 big.bin     # 큰 파일 없이: 마지막 인자 생략

전제: /api/ 는 tools/backend.py로 프록시(/api/slow는 10초 지연), /down/ 은 아무도 듣지 않는 포트.
각 케이스 뒤에 GET / 이 200인지(서버 생존)도 확인합니다. test_server.py와 달리 서버를 직접 띄우지 않으므로
ASan 빌드나 Linux 컨테이너 안의 서버에도 그대로 쓸 수 있습니다.
"""
import socket, sys, time, threading

PORT = int(sys.argv[1])
BIG = sys.argv[2] if len(sys.argv) > 2 else None
rows, fails = [], 0

def conn(timeout=8):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
    return s

def status_of(raw, timeout=8, close_write=False):
    s = conn(timeout)
    def _send():
        try: s.sendall(raw)
        except OSError: pass  # 서버가 큰 요청을 일찍 거절하고 닫으면 송신 쪽은 EPIPE/RST
    sender = threading.Thread(target=_send); sender.start()
    try:
        if close_write:
            sender.join()
            s.shutdown(socket.SHUT_WR)
        data = b""
        while b"\r\n" not in data:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        return data.split(b"\r\n")[0].decode(errors="replace") or "(closed, no response)"
    except ConnectionResetError:
        return "(RST)"
    except socket.timeout:
        return "(timeout)"
    finally:
        sender.join(); s.close()

def alive():
    try:
        return status_of(b"GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n").startswith("HTTP/1.1 200")
    except OSError:
        return False

def case(group, name, expect, actual):
    global fails
    ok = expect in actual and alive()
    fails += not ok
    rows.append((group, name, expect, actual, "PASS" if ok else "FAIL"))

H = b"Host: a\r\n"
# 1) 잘못된 HTTP 요청
case("malformed", "incomplete header + client closes", "400", status_of(b"GET / HTTP/1.1\r\nHo", close_write=True))
case("malformed", "unknown method FOO on static", "405", status_of(b"FOO / HTTP/1.1\r\n" + H + b"\r\n"))
case("malformed", "lowercase method 'get'", "405", status_of(b"get / HTTP/1.1\r\n" + H + b"\r\n"))
case("malformed", "method with '(' char", "400", status_of(b"GE(T / HTTP/1.1\r\n" + H + b"\r\n"))
case("malformed", "version HTTP/2.0", "505", status_of(b"GET / HTTP/2.0\r\n" + H + b"\r\n"))
case("malformed", "version HTTP/1.x", "400", status_of(b"GET / HTTP/1.x\r\n" + H + b"\r\n"))
case("malformed", "version missing", "400", status_of(b"GET /\r\n" + H + b"\r\n"))
case("malformed", "HTTP/1.1 without Host", "400", status_of(b"GET / HTTP/1.1\r\n\r\n"))
case("malformed", "binary garbage + NUL", "400", status_of(b"\x00\xff\x13garbage\r\n\r\n"))
case("malformed", "bare LF line endings", "400", status_of(b"GET / HTTP/1.1\nHost: a\n\n"))
case("malformed", "CL + TE (smuggling)", "400", status_of(b"POST / HTTP/1.1\r\n" + H + b"Content-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\nabc"))
case("malformed", "duplicate Content-Length", "400", status_of(b"POST / HTTP/1.1\r\n" + H + b"Content-Length: 1\r\nContent-Length: 2\r\n\r\nab"))
case("malformed", "Transfer-Encoding: chunked", "501", status_of(b"POST /api/x HTTP/1.1\r\n" + H + b"Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n"))
case("malformed", "encoded traversal %2e%2e", "403", status_of(b"GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\n" + H + b"\r\n"))
case("malformed", "bad percent %zz", "400", status_of(b"GET /%zz HTTP/1.1\r\n" + H + b"\r\n"))
# 2) 매우 큰 본문 / 매우 긴 URI / 헤더
case("size", "Content-Length 10MB (body not sent)", "413", status_of(b"POST /api/x HTTP/1.1\r\n" + H + b"Content-Length: 10485760\r\n\r\n"))
case("size", "Content-Length 2^70 overflow", "413", status_of(b"POST /api/x HTTP/1.1\r\n" + H + b"Content-Length: 1180591620717411303424\r\n\r\n"))
case("size", "URI 10,000 bytes", "414", status_of(b"GET /" + b"a" * 10000 + b" HTTP/1.1\r\n" + H + b"\r\n"))
case("size", "URI 1MB", "414", status_of(b"GET /" + b"a" * (1 << 20) + b" HTTP/1.1\r\n" + H + b"\r\n"))
case("size", "headers 64KB", "431", status_of(b"GET / HTTP/1.1\r\n" + H + b"X: " + b"b" * 65536 + b"\r\n\r\n"))
case("size", "body 64KB (at limit) via proxy", "200", status_of(b"POST /api/x HTTP/1.1\r\n" + H + b"Content-Length: 65536\r\n\r\n" + b"z" * 65536))
# 3) 백엔드 무응답 / 타임아웃
t = time.time(); r = status_of(b"GET /api/slow HTTP/1.1\r\n" + H + b"\r\n", timeout=15)
case("proxy", f"backend sleeps 10s -> {time.time()-t:.1f}s", "504", r)
case("proxy", "backend port closed", "502", status_of(b"GET /down/x HTTP/1.1\r\n" + H + b"\r\n"))
# 4) 클라이언트의 갑작스러운 종료
def abrupt(payload, rst):
    s = conn()
    if payload: s.sendall(payload)
    if rst:  # SO_LINGER 0 → close 시 RST 전송
        import struct; s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()
for i in range(50): abrupt(None, False)
case("disconnect", "50 connect-then-close (no bytes)", "-", "-")
for i in range(50): abrupt(b"POST /api/x HTTP/1.1\r\nHost: a\r\nContent-Length: 1000\r\n\r\nhalf", True)
case("disconnect", "50 RST mid-body (proxy route)", "-", "-")
for i in range(20): abrupt(b"GET /api/slow HTTP/1.1\r\nHost: a\r\n\r\n", True)
case("disconnect", "20 RST while waiting on slow backend", "-", "-")
if BIG:
    got = 0; s = conn(); s.sendall(b"GET /" + BIG.encode() + b" HTTP/1.1\r\nHost: a\r\n\r\n")
    while got < 1 << 20: got += len(s.recv(65536))
    import struct; s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)); s.close()
    case("disconnect", "RST after 1MB of 64MB download", "-", "-")
    s = conn(); s.sendall(b"GET /" + BIG.encode() + b" HTTP/1.1\r\nHost: a\r\n\r\n"); s.recv(100)
    t = time.time()  # 읽지 않고 버티기 → 서버 send 타임아웃(5s) 후 정리되어야 함
    time.sleep(7); s.close()
    case("disconnect", "client stops reading 64MB download (7s)", "-", "-")
# 5) slowloris: 1바이트씩 → 5초 뒤 408
s = conn(15); t = time.time(); # 다 보내려면 약 28초 → 10초 한도보다 충분히 길어야 "한도에서 끊기는지"를 시험할 수 있음
req = b"GET / HTTP/1.1\r\nHost: a\r\nX-Pad: " + b"p" * 40 + b"\r\n\r\n"; resp = b""
try:
    for b in req:
        s.sendall(bytes([b])); time.sleep(0.4)
        s.setblocking(False)
        try: resp += s.recv(100)
        except BlockingIOError: pass
        s.setblocking(True)
        if resp: break
except (BrokenPipeError, ConnectionResetError): pass
s.settimeout(3)
try: resp += s.recv(100)
except Exception: pass
s.close()
case("slow", f"slowloris 1B/0.4s -> answered at {time.time()-t:.1f}s", "408", resp.split(b"\r\n")[0].decode() or "(closed)")
# 6) 동시성: 정상 요청과 악성 요청 섞어서
errs = []
def worker(i):
    try:
        raw = b"GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n" if i % 2 else b"GARBAGE\r\n\r\n"
        r = status_of(raw); want = "200" if i % 2 else "400"
        if want not in r: errs.append((i, r))
    except Exception as e: errs.append((i, repr(e)))
ts = [threading.Thread(target=worker, args=(i,)) for i in range(200)]
[t.start() for t in ts]; [t.join() for t in ts]
case("concurrency", "200 threads, half garbage", "errors=0", f"errors={len(errs)}")

w = max(len(r[1]) for r in rows)
for g, n, e, a, ok in rows:
    print(f"{ok}  {g:<11} {n:<{w}}  expect={e:<9} got={a}")
print(f"\n{len(rows)-fails}/{len(rows)} edge cases passed (each followed by a liveness check)")
sys.exit(1 if fails else 0)
