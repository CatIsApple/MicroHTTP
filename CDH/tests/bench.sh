#!/bin/sh
# bench.sh — 부하 테스트: 동시 연결 수에 따른 처리량(req/s, MB/s)과 지연 시간(p50 / p99) 측정.
#
#   make bench                      (기본: 각 조합 3초)
#   DUR=10 CONCS="1 10 100" make bench
#
# 임시 문서 루트에 www/ 와 1MB / 10MB 파일을 만들고, 서버를 -q 로 띄워 tests/bench.c 로 측정한다.
# ab / wrk / hey 가 설치돼 있으면 같은 서버에 한 번씩 더 돌려 결과를 비교용으로 붙인다.
#   (Ubuntu: sudo apt install apache2-utils wrk)

set -eu

BIN=${BIN:-./microhttp}
BENCH=${BENCH:-./build/bench}
PORT=${PORT:-18080}
DUR=${DUR:-3}
CONCS=${CONCS:-"1 10 50 100"}
PATHS=${PATHS:-"/index.html /1m.bin /10m.bin"}

root=$(mktemp -d "${TMPDIR:-/tmp}/microhttp_bench_XXXXXX")
cp -R www/. "$root"/
head -c 1048576 /dev/urandom >"$root/1m.bin"
head -c 10485760 /dev/urandom >"$root/10m.bin"

"$BIN" -q -r "$root" "$PORT" >/dev/null &
pid=$!
cleanup() {
    kill -INT "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -rf "$root"
}
trap cleanup EXIT INT TERM

# 서버가 포트를 열 때까지 (최대 3초)
i=0
until "$BENCH" -c 1 -d 1 -p "$PORT" /index.html >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        echo "server did not start on port $PORT" >&2
        exit 1
    fi
    sleep 0.1
done

echo "server: $BIN (pid $pid), port $PORT, ${DUR}s per run, $(nproc 2>/dev/null || sysctl -n hw.ncpu) cpus"
printf "%-14s %5s %10s %9s %8s %8s %8s %8s %8s %6s\n" path conc "req/s" "MB/s" avg_ms p50_ms p99_ms max_ms ok fail
status=0
for p in $PATHS; do
    for c in $CONCS; do
        "$BENCH" -c "$c" -d "$DUR" -p "$PORT" "$p" || status=1   # 실패 요청이 있으면 종료 코드 2
    done
done

# 표준 도구가 있으면 비교용으로 (같은 서버, 같은 파일)
if command -v ab >/dev/null 2>&1; then
    echo
    echo "--- ab -n 5000 -c 50 /index.html"
    ab -q -n 5000 -c 50 "http://127.0.0.1:$PORT/index.html" | grep -E "Requests per second|Time per request|Failed requests|Transfer rate"
fi
if command -v wrk >/dev/null 2>&1; then
    echo
    echo "--- wrk -t4 -c50 -d${DUR}s /index.html"
    wrk -t4 -c50 -d"${DUR}s" "http://127.0.0.1:$PORT/index.html"
fi
if command -v hey >/dev/null 2>&1; then
    echo
    echo "--- hey -z ${DUR}s -c 50 /index.html"
    hey -z "${DUR}s" -c 50 "http://127.0.0.1:$PORT/index.html" | sed -n '1,12p'
fi

exit $status
