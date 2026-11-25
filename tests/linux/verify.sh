#!/bin/sh
# Linux에서 macOS로는 못 하던 검증을 한 번에 실행합니다.
#   docker build -t microhttp-verify tests/linux
#   docker run --rm -v "$PWD":/src:ro microhttp-verify sh /src/tests/linux/verify.sh
#
#  1. 일반 사용자(tester)로 make test  → 권한 403 단위 테스트까지 실제로 실행 (root는 권한 검사를 무시)
#  2. Valgrind memcheck로 단위 테스트 5개
#  3. Valgrind로 실제 서버 실행 + 엣지 탐침 30케이스 + SIGTERM → 누수·잘못된 접근·열린 fd 확인
#  4. malloc 실패 주입(LD_PRELOAD) → 연결 버퍼 할당 실패 시 500 응답 후 서버가 계속 동작하는지
set -u
FAIL=0
result() { printf 'RESULT %-4s %s\n' "$1" "$2"; [ "$1" = PASS ] || FAIL=1; }

W=/home/tester/w
rm -rf "$W" && cp -r /src/. "$W" && chown -R tester:tester "$W"
cd "$W"
as_tester() { runuser -u tester -- sh -c "cd $W && $1"; }

echo "== 1. make test as $(as_tester 'id -un') (gcc $(gcc -dumpversion))"
if as_tester 'make -s clean && make test' > /tmp/test.log 2>&1; then
    grep -E '^PASS test_[a-z]+:|tests passed' /tmp/test.log; result PASS "make test (unit + functional) as non-root"
else
    tail -40 /tmp/test.log; result FAIL "make test as non-root"
fi

echo "== 2. valgrind: unit tests"
VG="valgrind -q --error-exitcode=99 --leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite,indirect"
as_tester 'make -s clean && make -s unit CFLAGS="-std=c11 -O0 -g -Wall -Wextra -Wpedantic -Werror"' > /tmp/unit_build.log 2>&1 || { cat /tmp/unit_build.log; result FAIL "unit build"; }
for t in http mime static net proxy session; do
    if as_tester "$VG ./build/test_$t" > /tmp/vg_$t.log 2>&1; then
        result PASS "valgrind test_$t ($(grep -c . /tmp/vg_$t.log) lines, $(grep -c '==[0-9]*==' /tmp/vg_$t.log) valgrind reports)"
    else
        cat /tmp/vg_$t.log; result FAIL "valgrind test_$t"
    fi
done

echo "== 3. valgrind: live server + edge probe"
as_tester 'make -s clean && make -s server CFLAGS="-std=c11 -O0 -g -Wall -Wextra -Wpedantic -Werror"'
python3 tools/backend.py 19201 A >/dev/null 2>&1 & BACKEND=$!
runuser -u tester -- $VG --track-fds=yes --log-file=/tmp/vg_server.log \
    ./server -p 18200 -r ./www -U tests/functional/users.txt -R /api/=127.0.0.1:19201 -R /down/=127.0.0.1:19209 2> /tmp/server.log &
VG_JOB=$!   # runuser — 끝나면 valgrind의 종료 코드(오류가 있으면 99)를 그대로 돌려줌
for i in $(seq 1 60); do curl -s -o /dev/null http://127.0.0.1:18200/ && break; sleep 0.5; done
python3 tests/functional/edge_probe.py 18200 > /tmp/probe.log 2>&1; PROBE=$?
grep -E 'FAIL|passed' /tmp/probe.log
# 인증 경로도 Valgrind 아래에서: 로그인 → /me → 로그아웃 → /me
SID=$(curl -s -o /dev/null -D - -d 'username=alice&password=alice-test-pw' http://127.0.0.1:18200/login | tr -d '\r' | sed -n 's/^Set-Cookie: sid=\([0-9a-f]*\);.*/\1/p')
AUTH="$(curl -s -o /dev/null -w '%{http_code}' -b sid=$SID http://127.0.0.1:18200/me) $(curl -s -o /dev/null -w '%{http_code}' -d '' -b sid=$SID http://127.0.0.1:18200/logout) $(curl -s -o /dev/null -w '%{http_code}' -b sid=$SID http://127.0.0.1:18200/me)"
echo "auth under valgrind (me logout me): $AUTH"
[ "$AUTH" = "200 200 401" ] && result PASS "auth flow under valgrind" || result FAIL "auth flow under valgrind ($AUTH)"
# runuser의 자식인 valgrind.bin 프로세스에 직접 SIGTERM을 보내고 끝날 때까지 기다립니다.
SERVER_PID=$(pgrep -f 'valgrind.bin.*-p 18200' | head -1)
kill -TERM "$SERVER_PID"
for i in $(seq 1 120); do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 0.5; done
wait "$VG_JOB"; VG_EXIT=$?
kill "$BACKEND" 2>/dev/null
grep -E 'FILE DESCRIPTORS|ERROR SUMMARY|definitely|indirectly' /tmp/vg_server.log
[ "$PROBE" = 0 ] && result PASS "edge probe under valgrind" || result FAIL "edge probe under valgrind"
if [ "$VG_EXIT" = 0 ] && grep -q 'shutdown complete' /tmp/server.log && ! grep -qE 'Invalid|uninitialised|definitely lost: [1-9]|indirectly lost: [1-9]' /tmp/vg_server.log; then
    result PASS "valgrind live server: exit 0, no invalid access, no definite/indirect leak ($(grep -c '^\[request\]' /tmp/server.log) request log lines)"
else
    tail -60 /tmp/vg_server.log; result FAIL "valgrind live server (exit=$VG_EXIT)"
fi

echo "== 3b. valgrind: STEP 2 UAF experiment (unsafe = pointer, safe = copy)"
as_tester 'make -s build/step2_uaf CFLAGS="-std=c11 -O0 -g -Wall -Wextra -Wpedantic -Werror"' || result FAIL "step2_uaf build"
for mode in unsafe safe; do
    runuser -u tester -- valgrind -q --error-exitcode=99 ./build/step2_uaf $mode > /tmp/vg_uaf_$mode.log 2>&1; code=$?
    echo "$mode: exit=$code invalid_reads=$(grep -c 'Invalid read' /tmp/vg_uaf_$mode.log) $(grep -E '^(unsafe|safe):' /tmp/vg_uaf_$mode.log)"
done
grep -m1 -A4 'Invalid read' /tmp/vg_uaf_unsafe.log
[ "$(grep -c 'Invalid read' /tmp/vg_uaf_safe.log)" = 0 ] && result PASS "valgrind: safe (copy) has no invalid read" || result FAIL "valgrind: safe mode"

echo "== 4. malloc failure injection"
as_tester 'make -s clean && make -s server && gcc -shared -fPIC -o fail_malloc.so tests/linux/fail_malloc.c' || result FAIL "fail_malloc.so build"
FAIL_MALLOC_SIZE=81920 LD_PRELOAD=$W/fail_malloc.so runuser -u tester -- ./server -p 18300 -r ./www 2> /tmp/inject.log &
for i in $(seq 1 20); do curl -s -o /dev/null http://127.0.0.1:18300/ && break; sleep 0.2; done
CODES=$(for i in 1 2 3 4 5; do curl -s -o /dev/null -w '%{http_code} ' http://127.0.0.1:18300/; done)
INJECT_PID=$(pgrep -f '^./server -p 18300')
echo "status codes with malloc(81920) failing: $CODES"
if [ "$CODES" = "500 500 500 500 500 " ] && [ -n "$INJECT_PID" ]; then
    result PASS "malloc failure -> 500, server still running"
else
    result FAIL "malloc failure injection"
fi
kill -TERM "$INJECT_PID" 2>/dev/null; sleep 1

echo
[ "$FAIL" = 0 ] && echo "ALL LINUX CHECKS PASSED" || echo "SOME LINUX CHECKS FAILED"
exit "$FAIL"
