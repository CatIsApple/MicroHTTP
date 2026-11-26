#!/bin/sh
# 발표 데모 — 엔터를 누를 때마다 다음 단계로 넘어갑니다.
#
#   sh demo.sh            발표용 (단계마다 멈춤)
#   sh demo.sh --auto     멈추지 않고 끝까지 실행 (리허설 · 녹화용)
#
#   ① HTTP · Cookie  로그인 → 새 연결에서도 Session ID로 인식 → Cookie 없거나 위조하면 401
#   ② 소켓           닫힌 FD가 재사용되면, FD로 식별하는 서버는 로그인하지 않은 B를 userA로 착각
#   ③ 스레드 풀       공유 메모리를 동시에 지울 때 — 포인터는 ASan이 검출, 복사는 0건
#   ④ 검증           Session 단위 테스트 + 인증 기능 테스트 6개
set -u
cd "$(dirname "$0")"
AUTO=0
[ "${1:-}" = "--auto" ] && AUTO=1
PORT=18888
JAR=/tmp/microhttp_demo_cookie

BOLD=$(printf '\033[1m'); BLUE=$(printf '\033[34m'); RED=$(printf '\033[31m')
GREEN=$(printf '\033[32m'); DIM=$(printf '\033[2m'); RESET=$(printf '\033[0m')

title() { printf '\n%s━━━ %s ━━━%s\n' "$BOLD" "$1" "$RESET"; }
pause() { [ "$AUTO" = 1 ] && return; printf '%s(엔터)%s' "$DIM" "$RESET"; read -r _; }
show()  { printf '%s$ %s%s\n' "$BLUE" "$1" "$RESET"; }
ok()    { printf '  %s%s%s\n' "$GREEN" "$1" "$RESET"; }
bad()   { printf '  %s%s%s\n' "$RED" "$1" "$RESET"; }

# ── 준비: 발표 전에 미리 빌드 (데모 중에는 컴파일하지 않음) ──
printf '%s준비: 빌드 중...%s\n' "$DIM" "$RESET"
make -s server build/server_fd_demo build/test_session >/dev/null || exit 1
rm -f build/step2_uaf
make -s build/step2_uaf CFLAGS='-std=c11 -O1 -g -fsanitize=address -fno-omit-frame-pointer' \
     LDFLAGS='-fsanitize=address' >/dev/null || exit 1
./server -p $PORT -U tests/functional/users.txt 2>/dev/null & SERVER=$!
trap 'kill $SERVER 2>/dev/null; rm -f $JAR' EXIT
sleep 0.5
printf '%s준비 완료%s\n' "$DIM" "$RESET"
pause

# ── ① 인증 흐름 ──
title "① HTTP · Cookie — 새 연결에서도 Session ID로 인식"
show "curl -d 'username=userA&password=…' /login"
curl -s -o /dev/null -D - -c $JAR -d 'username=userA&password=userA-test-pw' http://127.0.0.1:$PORT/login \
    | tr -d '\r' | sed -n -e '1p' -e 's/^\(Set-Cookie: sid=.\{12\}\).*\(; Path.*\)/\1…\2/p' | while read -r line; do ok "$line"; done
pause
show "curl -b cookie /me          # 새 TCP 연결"
ok "$(curl -s -w '  (%{http_code})' -b $JAR http://127.0.0.1:$PORT/me | tr -d '\n')"
show "curl /me                    # Cookie 없이"
bad "$(curl -s -w '  (%{http_code})' http://127.0.0.1:$PORT/me | tr -d '\n')"
show "curl -b 'user=userA; logged_in=true' /me   # 위조 Cookie"
bad "$(curl -s -w '  (%{http_code})' -b 'user=userA; logged_in=true' http://127.0.0.1:$PORT/me | tr -d '\n')"
pause

# ── ② FD 재사용 ──
title "② 소켓 — 닫힌 FD가 다음 연결에 재사용되면"
show "python3 tests/experiments/step1_fd_reuse.py"
python3 tests/experiments/step1_fd_reuse.py | while IFS= read -r line; do
    case "$line" in
        "## "*)    printf '  %s\n' "${line#\#\# }" ;;
        "서버 로그의 연결 fd 번호: "*)   # 커널이 같은 FD를 다시 줬다는 서버 쪽 증거
            fd=$(printf '%s' "$line" | sed 's/^서버 로그의 연결 fd 번호: \([0-9, ]*\).*/\1/; s/ *$//')
            case "$fd" in
                *,*) printf '    %s서버 로그: 연결 fd %s%s\n' "$DIM" "$fd" "$RESET" ;;
                *)   printf '    %s서버 로그: 세 연결 모두 fd %s%s\n' "$DIM" "$fd" "$RESET" ;;
            esac ;;
        *"2) B"*"userA"*) bad "${line}" ;;
        *"2) B"*)         ok "${line}" ;;
    esac
done
pause

# ── ③ UAF ──
title "③ 스레드 풀 — 공유 메모리를 동시에 지우면 (AddressSanitizer)"
show "build/step2_uaf unsafe      # 조회 결과를 포인터로 넘김"
build/step2_uaf unsafe 2>&1 | grep -m1 'ERROR: AddressSanitizer' | sed 's/^==[0-9]*==//; s/ on address.*//' | while read -r line; do bad "$line"; done
build/step2_uaf unsafe 2>&1 | grep -m1 'in session_destroy' | sed 's/.* in /  freed by: /; s/ (.*//' | while read -r line; do bad "$line"; done
pause
show "build/step2_uaf safe        # 락 안에서 값을 복사"
ok "$(build/step2_uaf safe 2>&1 | tail -1)  → ASan 보고 0건"
pause

# ── ④ 테스트 ──
title "④ 검증 — 단위 · 기능 테스트"
show "build/test_session          # 만료 · 위조 ID · 동시 접근 단위 테스트"
ok "$(build/test_session)"
show "python3 tests/functional/test_server.py auth   # 실제 TCP로 인증 기능 테스트"
python3 tests/functional/test_server.py auth 2>&1 | grep -E '^(PASS|FAIL)|passed' | while read -r line; do
    case "$line" in FAIL*) bad "$line" ;; *) ok "$line" ;; esac
done
printf '\n%s전체: make test (단위 6 + 기능 34) · make asan · make tsan · tests/linux/verify.sh (Valgrind)%s\n' "$DIM" "$RESET"
