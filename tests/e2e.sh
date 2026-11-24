#!/usr/bin/env bash
# e2e.sh — 서버를 띄우고 실제 소켓으로 요청을 보내 응답 상태 줄을 확인한다.
# 사용법: tests/e2e.sh ./microhttp [port]
set -u
BIN=${1:-./microhttp}
PORT=${2:-18080}
LOG=$(mktemp)
pass=0 fail=0

"$BIN" -q "$PORT" >"$LOG" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -f "$LOG"' EXIT
for _ in $(seq 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break; sleep 0.1; done

check() { # check <설명> <기대값 앞부분> <실제값>
    if [[ "$3" == "$2"* ]]; then echo "  ok   $1"; pass=$((pass + 1))
    else echo "  FAIL $1: want '$2', got '$3'"; fail=$((fail + 1)); fi
}
status_of() { head -1 | tr -d '\r'; }
send_raw() { printf "$1" | nc -q 2 127.0.0.1 "$PORT" | status_of; }

check "curl GET /"      "HTTP/1.1 200" "$(curl -s -i "http://127.0.0.1:$PORT/" | status_of)"
check "curl body"       "Hello from MicroHTTP" "$(curl -s "http://127.0.0.1:$PORT/")"
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
check "curl POST form"            "HTTP/1.1 200" "$(curl -s -i -d 'name=da0nn' "http://127.0.0.1:$PORT/form" | status_of)"
check "curl POST 500KB"           "HTTP/1.1 200" \
    "$(head -c 500000 /dev/zero | curl -s -i --data-binary @- "http://127.0.0.1:$PORT/upload" | status_of)"
check "POST body arrives in two pieces" "HTTP/1.1 200" \
    "$( { printf 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nhello'; sleep 0.3; printf 'world'; } | nc -q 2 127.0.0.1 "$PORT" | status_of)"
check "body shorter than Content-Length -> 400" "HTTP/1.1 400" \
    "$(send_raw 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc')"
head_resp=$(curl -s -I "http://127.0.0.1:$PORT/" | tr -d '\r')
if [[ "$head_resp" == "HTTP/1.1 200"* && "$head_resp" != *Hello* ]]; then echo "  ok   HEAD has no body"; pass=$((pass + 1))
else echo "  FAIL HEAD: '$head_resp'"; fail=$((fail + 1)); fi

# ---- L3 응답 ----
resp=$(printf 'HELLO\r\n\r\n' | nc -q 2 127.0.0.1 "$PORT" | tr -d '\r')
check "error page is HTML" "yes" "$([[ "$resp" == *"Content-Type: text/html"* && "$resp" == *"<h1>400 Bad Request</h1>"* ]] && echo yes)"
check "Date and Server headers" "yes" "$(curl -s -I "http://127.0.0.1:$PORT/" | grep -c -E '^(Date|Server):' | grep -q 2 && echo yes)"

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

if grep -qE "AddressSanitizer|runtime error" "$LOG"; then echo "  FAIL sanitizer report:"; cat "$LOG"; fail=$((fail + 1)); fi
echo "e2e: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
