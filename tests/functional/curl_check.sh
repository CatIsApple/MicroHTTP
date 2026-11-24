#!/bin/sh
# curl로 정적 파일과 Reverse Proxy 동작을 확인합니다.
#   sh tests/functional/curl_check.sh
# 서버(18090)와 백엔드 2개(19101, 19102)를 띄우고, 각 요청의 상태 코드와 핵심 헤더를 출력한 뒤 종료합니다.
set -u
cd "$(dirname "$0")/../.."
make -s server || exit 1
PORT=18090
python3 tools/backend.py 19101 A 2>/dev/null & A=$!
python3 tools/backend.py 19102 B 2>/dev/null & B=$!
./server -p $PORT -r ./www -R /api/=127.0.0.1:19101,127.0.0.1:19102 -R /down/=127.0.0.1:19109 2>/tmp/microhttp_curl.log & S=$!
trap 'kill $S $A $B 2>/dev/null; wait 2>/dev/null' EXIT
sleep 1
URL=http://127.0.0.1:$PORT
FAIL=0

check() {  # check <설명> <기대 상태> <curl 인자...>
    name=$1; expected=$2; shift 2
    actual=$(curl -s -o /dev/null -w '%{http_code} %{content_type}' "$@")
    code=${actual%% *}
    if [ "$code" = "$expected" ]; then result=PASS; else result=FAIL; FAIL=1; fi
    printf '%s  %-38s -> %s\n' "$result" "$name" "$actual"
}

check "GET / (index.html)"            200 "$URL/"
check "GET /css/style.css"            200 "$URL/css/style.css"
check "GET /js/app.js"                200 "$URL/js/app.js"
check "GET /images/logo.png"          200 "$URL/images/logo.png"
check "GET /images/photo.jpg"         200 "$URL/images/photo.jpg"
check "HEAD /index.html"              200 -I "$URL/index.html"
check "GET /missing (404)"            404 "$URL/missing"
check "GET /../ traversal (403)"      403 --path-as-is "$URL/../readme.md"
check "POST static (405)"             405 -d x "$URL/index.html"
check "HTTP/1.0 GET"                  200 --http1.0 "$URL/"
check "GET /api/hello (proxy)"        200 "$URL/api/hello"
check "POST /api/echo (proxy body)"   200 -H 'Content-Type: text/plain' --data-binary 'hello' "$URL/api/echo"
check "GET /down/x (502)"             502 "$URL/down/x"
echo "--- round-robin (backend names):"
for i in 1 2 3 4; do curl -s "$URL/api/rr" | python3 -c 'import json,sys; print(" ", json.load(sys.stdin)["backend"])'; done
echo "--- keep-alive (curl reuses one connection for two URLs):"
curl -sv "$URL/css/style.css" "$URL/js/app.js" -o /dev/null -o /dev/null 2>&1 | grep -E 'Re-using|Connection: keep-alive|left intact' | sed 's/^/  /'
exit $FAIL
