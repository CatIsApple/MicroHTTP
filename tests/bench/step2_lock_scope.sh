#!/bin/sh
# STEP 2 실험: 락 범위 비교 — (a) 넓은 락: 응답 생성·send()까지 락 안  (b) 좁은 락: 복사 후 바로 해제
#
#   sh tests/bench/step2_lock_scope.sh [반복=3] > results.csv     (stderr: 락 통계)
#
# 같은 기계·같은 요청 수·같은 동시성. 두 빌드 모두 -DSESSION_EXPERIMENTS(락 통계)를 켜서 계측 비용도 같게 합니다.
# 요청: 로그인한 Cookie로 GET /me (keep-alive), 실행마다 TIME_WAIT 식힘.
set -u
cd "$(dirname "$0")/../.."
RUNS=${1:-3}
PORT=18810
make -s build/server_narrow_lock build/server_wide_lock || exit 1
time_wait_count() { netstat -an 2>/dev/null | grep -c TIME_WAIT || true; }
cool_down() { while [ "$(time_wait_count)" -gt 1000 ]; do sleep 2; done; }

echo "scenario,run,requests_per_sec,avg_ms,p95_ms,p99_ms,status_200,non_200_or_error"
for variant in narrow wide; do
    build/server_${variant}_lock -p $PORT -r ./www -U tests/functional/users.txt 2> /tmp/microhttp_lock_$variant.log & SERVER=$!
    sleep 0.5
    SID=$(curl -s -o /dev/null -D - -d 'username=userA&password=userA-test-pw' http://127.0.0.1:$PORT/login \
          | sed -n 's/^Set-Cookie: sid=\([0-9a-f]*\);.*/\1/p')
    for concurrency in 8 50; do
        run=1
        while [ "$run" -le "$RUNS" ]; do
            cool_down
            hey -o csv -n 20000 -c $concurrency -H "Cookie: sid=$SID" http://127.0.0.1:$PORT/me \
                | python3 tests/bench/hey_stats.py "${variant}_lock_c$concurrency" "$run" 20000
            run=$((run + 1))
        done
    done
    kill -TERM $SERVER; wait $SERVER 2>/dev/null
    printf '%s: ' "$variant" >&2; grep '\[session-lock\]' /tmp/microhttp_lock_$variant.log >&2
done
