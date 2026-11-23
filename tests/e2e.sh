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
