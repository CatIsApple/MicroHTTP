#!/bin/sh
# 성능 베이스라인 측정 — 인증 기능을 넣기 전/후를 같은 조건으로 비교하기 위한 스크립트
#
#   sh tests/bench/baseline.sh [반복횟수=3] > results.csv
#
# - release 빌드(-O2), 작업 스레드 8(기본값), 백엔드 1개(tools/backend.py)
# - 도구: hey -o csv (요청별 원시 시간) → tests/bench/hey_stats.py 로 계산
#   한계: hey가 기록하는 시간 해상도가 0.1ms라 1ms 미만 지연의 백분위는 0.1ms 단위로 끊깁니다
# - 실행마다 TIME_WAIT 소켓이 1000개 아래로 줄 때까지 기다립니다.
#   (루프백 한 대에서 새 연결을 대량으로 만들면 ephemeral 포트가 고갈되어 지연이 튀는 것을
#    QUALITY_REPORT 3절에서 확인했기 때문에, 측정값이 이전 실행의 영향을 받지 않게 합니다)
# - 출력(stdout): CSV 한 줄 = 시나리오 1회 실행.  요약: python3 tests/bench/summarize.py results.csv
set -u
cd "$(dirname "$0")/../.."
RUNS=${1:-3}
PORT=18800
BACKEND_PORT=19801
URL=http://127.0.0.1:$PORT

make -s clean && make -s server || exit 1
python3 tools/backend.py $BACKEND_PORT A >/dev/null 2>&1 & BACKEND=$!
./server -p $PORT -r ./www -R /api/=127.0.0.1:$BACKEND_PORT 2>/dev/null & SERVER=$!
trap 'kill $SERVER $BACKEND 2>/dev/null; wait 2>/dev/null' EXIT
sleep 1

time_wait_count() { netstat -an 2>/dev/null | grep -c TIME_WAIT || true; }  # 0개면 grep이 1을 반환하므로 || true
cool_down() {
    while [ "$(time_wait_count)" -gt 1000 ]; do sleep 2; done
}
fd_count() { lsof -p "$SERVER" 2>/dev/null | wc -l | tr -d ' '; }

# 시나리오: 이름|hey 인자
SCENARIOS='static_keepalive_c8|-n 20000 -c 8 URL/
static_keepalive_c50|-n 20000 -c 50 URL/
static_newconn_c10|-n 3000 -c 10 -disable-keepalive URL/
static_newconn_c50|-n 3000 -c 50 -disable-keepalive URL/
proxy_c10|-n 1000 -c 10 URL/api/x'

echo "fd_before=$(fd_count)" >&2
echo "scenario,run,requests_per_sec,avg_ms,p95_ms,p99_ms,status_200,non_200_or_error"
echo "$SCENARIOS" | while IFS='|' read -r name args; do
    count=$(echo "$args" | sed 's/.*-n \([0-9]*\).*/\1/')
    run=1
    while [ "$run" -le "$RUNS" ]; do
        cool_down
        # shellcheck disable=SC2086
        hey -o csv $(echo "$args" | sed "s|URL|$URL|") | python3 tests/bench/hey_stats.py "$name" "$run" "$count"
        run=$((run + 1))
    done
done
sleep 6   # keep-alive 연결이 서버 쪽에서 정리될 시간 (유휴 타임아웃 5초)
echo "fd_after=$(fd_count)" >&2
