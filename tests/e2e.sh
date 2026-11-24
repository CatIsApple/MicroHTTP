#!/usr/bin/env bash
# e2e.sh — 서버를 띄우고 실제 소켓으로 요청을 보내 응답 상태 줄을 확인한다.
# 사용법: tests/e2e.sh ./microhttp [port]
set -u
BIN=${1:-./microhttp}
PORT=${2:-18080}
LOG=$(mktemp)
ROOT=$(mktemp -d) # www/ 사본 + 테스트용 심볼릭 링크 (저장소의 www/ 는 건드리지 않음)
cp -R www/. "$ROOT"/
ln -s /etc "$ROOT/etc-link"
pass=0 fail=0

"$BIN" -q -r "$ROOT" -c cgi-bin "$PORT" >"$LOG" 2>&1 &
pid=$!
pid2=
trap 'kill $pid $pid2 2>/dev/null; wait 2>/dev/null; rm -rf "$LOG" "$ROOT"' EXIT
for _ in $(seq 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break; sleep 0.1; done

check() { # check <설명> <기대값 앞부분> <실제값>
    if [[ "$3" == "$2"* ]]; then echo "  ok   $1"; pass=$((pass + 1))
    else echo "  FAIL $1: want '$2', got '$3'"; fail=$((fail + 1)); fi
}
status_of() { head -1 | tr -d '\r'; }
send_raw() { printf "$1" | nc -q 2 127.0.0.1 "$PORT" | status_of; }

check "curl GET /"      "HTTP/1.1 200" "$(curl -s -i "http://127.0.0.1:$PORT/" | status_of)"
check "curl body is index.html" "yes" "$(curl -s "http://127.0.0.1:$PORT/" | grep -q '<h1>MicroHTTP</h1>' && echo yes)"
check "request arrives in two pieces" "HTTP/1.1 200" \
    "$( { printf 'GET / HTTP/1.1\r\nHo'; sleep 0.3; printf 'st: x\r\n\r\n'; } | nc -q 2 127.0.0.1 "$PORT" | status_of)"
check "closed before blank line -> 400" "HTTP/1.1 400" "$(send_raw 'GET / HTTP/1.1\r\nHost: x\r\n')"
check "header over 8KB -> 431" "HTTP/1.1 431" \
    "$(send_raw "GET / HTTP/1.1\r\nX: $(head -c 9000 /dev/zero | tr '\0' a)\r\n\r\n")"

# ---- L2 파서 ----
check "garbage -> 400"            "HTTP/1.1 400" "$(send_raw 'HELLO\r\n\r\n')"
check "HTTP/1.1 without Host -> 400" "HTTP/1.1 400" "$(send_raw 'GET / HTTP/1.1\r\n\r\n')"
check "HTTP/1.0 without Host -> 200" "HTTP/1.1 200" "$(send_raw 'GET / HTTP/1.0\r\n\r\n')"
check "PUT -> 501"                "HTTP/1.1 501" "$(send_raw 'PUT / HTTP/1.1\r\nHost: x\r\n\r\n')"
check "HTTP/2.0 -> 505"           "HTTP/1.1 505" "$(send_raw 'GET / HTTP/2.0\r\nHost: x\r\n\r\n')"
check "bad Content-Length -> 400" "HTTP/1.1 400" "$(send_raw 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n')"
check "body over 1MB -> 413"      "HTTP/1.1 413" "$(send_raw 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n')"
check "path with .. -> 400"       "HTTP/1.1 400" "$(curl -s -i --path-as-is "http://127.0.0.1:$PORT/../etc/passwd" | status_of)"
check "curl POST to static file -> 405" "HTTP/1.1 405" "$(curl -s -i -d 'name=da0nn' "http://127.0.0.1:$PORT/form" | status_of)"
check "curl POST 500KB (read, then 405)" "HTTP/1.1 405" \
    "$(head -c 500000 /dev/zero | curl -s -i --data-binary @- "http://127.0.0.1:$PORT/upload" | status_of)"
check "POST body arrives in two pieces" "HTTP/1.1 405" \
    "$( { printf 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nhello'; sleep 0.3; printf 'world'; } | nc -q 2 127.0.0.1 "$PORT" | status_of)"
check "body shorter than Content-Length -> 400" "HTTP/1.1 400" \
    "$(send_raw 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc')"
head_resp=$(curl -s -I "http://127.0.0.1:$PORT/" | tr -d '\r')
if [[ "$head_resp" == "HTTP/1.1 200"* && "$head_resp" != *"<h1>"* ]]; then echo "  ok   HEAD has no body"; pass=$((pass + 1))
else echo "  FAIL HEAD: '$head_resp'"; fail=$((fail + 1)); fi

# ---- L3 응답 ----
resp=$(printf 'HELLO\r\n\r\n' | nc -q 2 127.0.0.1 "$PORT" | tr -d '\r')
check "error page is HTML" "yes" "$([[ "$resp" == *"Content-Type: text/html"* && "$resp" == *"<h1>400 Bad Request</h1>"* ]] && echo yes)"
check "Date and Server headers" "yes" "$(curl -s -I "http://127.0.0.1:$PORT/" | grep -c -E '^(Date|Server):' | grep -q 2 && echo yes)"

# ---- L4 정적 파일 ----
ctype() { curl -s -o /dev/null -w '%{content_type}' "http://127.0.0.1:$PORT$1"; }
code() { curl -s -o /dev/null --path-as-is -w '%{http_code}' "http://127.0.0.1:$PORT$1"; }
check "index.html type"  "text/html"       "$(ctype /index.html)"
check "style.css type"   "text/css"        "$(ctype /style.css)"
check "app.js type"      "text/javascript" "$(ctype /app.js)"
check "logo.png type"    "image/png"       "$(ctype /img/logo.png)"
check "png bytes match"  "yes" "$(curl -s "http://127.0.0.1:$PORT/img/logo.png" | cmp -s - www/img/logo.png && echo yes)"
check "dir without slash -> 301" "301 /docs/" \
    "$(curl -s -o /dev/null -w '%{http_code} %{redirect_url}' "http://127.0.0.1:$PORT/docs" | sed "s#http://127.0.0.1:$PORT##")"
check "dir/ -> index.html" "200" "$(code /docs/)"
check "missing file -> 404" "404" "$(code /nope.html)"
check "dir without index -> 403" "403" "$(code /img/)"
check "symlink out of root -> 403" "403" "$(code /etc-link/passwd)"
check "encoded space in path -> 404" "404" "$(code '/no%20such')"
check "HEAD on file" "HTTP/1.1 200" "$(curl -s -I "http://127.0.0.1:$PORT/style.css" | status_of)"

# ---- L5 CGI ----
url="http://127.0.0.1:$PORT"
out=$(curl -s "$url/cgi-bin/hello.sh/extra/path?name=da0nn")
check "CGI GET body"      "Hello from CGI" "$out"
check "CGI QUERY_STRING"  "yes" "$([[ "$out" == *"QUERY_STRING=name=da0nn"* ]] && echo yes)"
check "CGI PATH_INFO"     "yes" "$([[ "$out" == *"PATH_INFO=/extra/path"* && "$out" == *"SCRIPT_NAME=/cgi-bin/hello.sh"* ]] && echo yes)"
check "CGI REMOTE_ADDR"   "yes" "$([[ "$out" == *"REMOTE_ADDR=127.0.0.1"* ]] && echo yes)"
check "CGI POST echo"     "method=POST" "$(curl -s -d 'msg=hello' "$url/cgi-bin/echo.py")"
check "CGI POST body"     "yes" "$(curl -s -d 'msg=hello' "$url/cgi-bin/echo.py" | grep -qx 'body=msg=hello' && echo yes)"
check "CGI POST 500KB"    "X-Body-Length: 500000" \
    "$(head -c 500000 /dev/zero | curl -s -D - -o /dev/null --data-binary @- "$url/cgi-bin/echo.py" | grep -i x-body-length | tr -d '\r')"
check "CGI Status header -> 404" "404" "$(code /cgi-bin/status.sh)"
check "CGI Location -> 302" "302" "$(code /cgi-bin/redirect.sh)"
check "CGI missing -> 404"   "404" "$(code /cgi-bin/nope.sh)"
check "CGI not executable -> 403" "403" "$(code /cgi-bin/noexec.sh)"
check "CGI bad output -> 502" "502" "$(code /cgi-bin/broken.sh)"
check "CGI prefix itself -> 403" "403" "$(code /cgi-bin/)"
start=$(date +%s)
check "CGI timeout -> 504"   "504" "$(code /cgi-bin/slow.sh)"
took=$(( $(date +%s) - start ))
check "CGI timeout took ~5s" "yes" "$([[ $took -ge 4 && $took -le 7 ]] && echo yes)"

# ---- L6 keep-alive ----
reuse=$(curl -s -v -o /dev/null -o /dev/null "$url/index.html" "$url/style.css" 2>&1 | grep -c -i 're-using\|reusing')
check "curl reuses one connection" "yes" "$([[ $reuse -ge 1 ]] && echo yes)"
two=$(printf 'GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\nGET /style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' \
      | nc -q 3 127.0.0.1 "$PORT" | grep -c '^HTTP/1.1 200')
check "pipelined two requests -> two 200" "2" "$two"
check "Connection: close is honored" "Connection: close" \
    "$(curl -s -I -H 'Connection: close' "$url/" | grep -i '^connection:' | tr -d '\r')"
check "HTTP/1.1 default keep-alive" "Connection: keep-alive" \
    "$(curl -s -I "$url/" | grep -i '^connection:' | tr -d '\r')"
check "HTTP/1.0 closes" "Connection: close" \
    "$(curl -s -I -0 "$url/" | grep -i '^connection:' | tr -d '\r')"
check "error closes connection" "1" \
    "$(printf 'BAD\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n' | nc -q 3 127.0.0.1 "$PORT" | grep -c '^HTTP/1.1')"

# ---- L6 동시성 ----
# 느린 CGI(5초)가 워커 하나를 잡고 있어도 다른 요청은 바로 처리된다
curl -s -o /dev/null "$url/cgi-bin/slow.sh" &
slow_pid=$!
sleep 0.3
t0=$(date +%s%N)
check "served while slow CGI runs" "200" "$(code /hello.txt)"
ms=$(( ($(date +%s%N) - t0) / 1000000 ))
check "...within 1s" "yes" "$([[ $ms -lt 1000 ]] && echo yes)"
wait $slow_pid

# 동시에 50개 연결로 요청 400개
ok=$(python3 - "$PORT" <<'PY'
import sys, socket, threading
port = int(sys.argv[1]); ok = 0; lock = threading.Lock()
def worker():
    global ok
    for _ in range(8):
        s = socket.create_connection(("127.0.0.1", port), timeout=10)
        s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        data = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        s.close()
        if data.startswith(b"HTTP/1.1 200") and data.endswith(b"hello from a static file\n"):
            with lock:
                ok += 1
ts = [threading.Thread(target=worker) for _ in range(50)]
[t.start() for t in ts]; [t.join() for t in ts]
print(ok)
PY
)
check "50 concurrent clients, 400 requests" "400" "$ok"

# 워커 1개 + 대기열 16칸 서버에 아무것도 안 보내는 연결 24개 → 넘친 연결은 503
"$BIN" -q -w 1 -r "$ROOT" "$((PORT + 1))" >>"$LOG" 2>&1 &
pid2=$!
sleep 0.3
busy=$(python3 - "$((PORT + 1))" <<'PY'
import sys, socket, time
port = int(sys.argv[1]); socks = []
for _ in range(24):
    socks.append(socket.create_connection(("127.0.0.1", port)))
    time.sleep(0.02)
n = 0
for s in socks:
    s.settimeout(0.5)
    try:
        if s.recv(64).startswith(b"HTTP/1.1 503"):
            n += 1
    except socket.timeout:
        pass
print(n)
PY
)
check "queue full -> 503" "yes" "$([[ $busy -ge 1 ]] && echo yes)"

# 우아한 종료: SIGTERM 을 받아도 처리 중인 요청은 응답을 받고 나서 끝난다
sleep 0.5 # 위 연결들이 닫혀 워커가 비도록
inflight=$(mktemp)
( { printf 'GET /hello.txt HTTP/1.1\r\nHost: x\r\n'; sleep 1; printf 'Connection: close\r\n\r\n'; } \
    | nc -q 2 127.0.0.1 "$((PORT + 1))" | head -1 | tr -d '\r' >"$inflight" ) &
nc_pid=$!
sleep 0.4
kill -TERM $pid2
wait $nc_pid 2>/dev/null
check "in-flight request finished after SIGTERM" "HTTP/1.1 200" "$(cat "$inflight")"
for _ in $(seq 60); do kill -0 $pid2 2>/dev/null || break; sleep 0.1; done
check "server exited" "yes" "$(kill -0 $pid2 2>/dev/null || echo yes)"
rm -f "$inflight"

# 헤더를 끝내지 않고 버티는 클라이언트는 마감(5초)에 응답 없이 끊긴다
slow=$(python3 -c '
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(b"GET / HTTP/1.1\r\n")
t = time.time(); s.settimeout(10)
data = s.recv(100)
print("%.1f %d" % (time.time() - t, len(data)))
' "$PORT" 2>&1)
if [[ "$slow" =~ ^[4-6]\.[0-9]\ 0$ ]]; then echo "  ok   slow client cut off (${slow% *}s)"; pass=$((pass + 1))
else echo "  FAIL slow client: '$slow'"; fail=$((fail + 1)); fi

check "server still serving" "HTTP/1.1 200" "$(curl -s -i "http://127.0.0.1:$PORT/" | status_of)"

if grep -qE "AddressSanitizer|runtime error|ThreadSanitizer|LeakSanitizer" "$LOG"; then echo "  FAIL sanitizer report:"; cat "$LOG"; fail=$((fail + 1)); fi
echo "e2e: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
