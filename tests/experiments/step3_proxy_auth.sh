#!/bin/sh
# STEP 3 실험: Reverse Proxy를 거쳐도 Cookie 기반 인증이 그대로 동작하는가
#
#   make server && sh tests/experiments/step3_proxy_auth.sh
#
# 백엔드 B: 인증을 처리하는 MicroHTTP (18831, -U users.txt)
# 프록시 F: 인증 경로(/login, /logout, /me)를 B로 넘기는 MicroHTTP (18830, 사용자 파일 없음)
set -u
cd "$(dirname "$0")/../.."
B=18831; F=18830
./server -p $B -r ./www -U tests/functional/users.txt 2>/tmp/microhttp_step3_backend.log & BP=$!
./server -p $F -r ./www -R /login=127.0.0.1:$B -R /logout=127.0.0.1:$B -R /me=127.0.0.1:$B 2>/tmp/microhttp_step3_proxy.log & FP=$!
trap 'kill $BP $FP 2>/dev/null; wait 2>/dev/null' EXIT
sleep 0.5

login() {  # login <port> → sid 출력, 헤더는 /tmp/step3_headers
    curl -s -o /dev/null -D /tmp/step3_headers -d 'username=alice&password=alice-test-pw' http://127.0.0.1:$1/login
    sed -n 's/^Set-Cookie: sid=\([0-9a-f]*\);.*/\1/p' /tmp/step3_headers | tr -d '\r'
}
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

echo "| 확인 | 직접(B:$B) | 프록시 경유(F:$F) |"
echo "|---|---|---|"
SID_D=$(login $B); HD=$(tr -d '\r' < /tmp/step3_headers)
SID_P=$(login $F); HP=$(tr -d '\r' < /tmp/step3_headers)
echo "| 로그인 상태 코드 | $(echo "$HD" | head -1) | $(echo "$HP" | head -1) |"
echo "| Set-Cookie 속성 | $(echo "$HD" | grep -o 'Path=/; HttpOnly; SameSite=Lax') | $(echo "$HP" | grep -o 'Path=/; HttpOnly; SameSite=Lax') |"
echo "| sid 길이 | ${#SID_D} | ${#SID_P} |"
echo "| Cache-Control | $(echo "$HD" | sed -n 's/^Cache-Control: //p') | $(echo "$HP" | sed -n 's/^Cache-Control: //p') |"
echo "| 응답 Connection | $(echo "$HD" | sed -n 's/^Connection: //p') | $(echo "$HP" | sed -n 's/^Connection: //p') |"
echo "| /me (Cookie 없음) | $(code http://127.0.0.1:$B/me) | $(code http://127.0.0.1:$F/me) |"
echo "| /me (자기 경로에서 받은 sid) | $(code -b sid=$SID_D http://127.0.0.1:$B/me) | $(code -b sid=$SID_P http://127.0.0.1:$F/me) |"
echo "| /me (다른 경로에서 받은 sid) | $(code -b sid=$SID_P http://127.0.0.1:$B/me) | $(code -b sid=$SID_D http://127.0.0.1:$F/me) |"
echo "| /me 본문 | $(curl -s -b sid=$SID_D http://127.0.0.1:$B/me) | $(curl -s -b sid=$SID_P http://127.0.0.1:$F/me) |"
echo "| /me Cache-Control | $(curl -s -o /dev/null -D - -b sid=$SID_D http://127.0.0.1:$B/me | tr -d '\r' | sed -n 's/^Cache-Control: //p') | $(curl -s -o /dev/null -D - -b sid=$SID_P http://127.0.0.1:$F/me | tr -d '\r' | sed -n 's/^Cache-Control: //p') |"
LO_D=$(curl -s -o /dev/null -D - -d '' -b sid=$SID_D http://127.0.0.1:$B/logout | tr -d '\r' | grep -o 'Max-Age=0')
LO_P=$(curl -s -o /dev/null -D - -d '' -b sid=$SID_P http://127.0.0.1:$F/logout | tr -d '\r' | grep -o 'Max-Age=0')
echo "| 로그아웃 Set-Cookie | $LO_D | $LO_P |"
echo "| 로그아웃 후 /me (옛 sid) | $(code -b sid=$SID_D http://127.0.0.1:$B/me) | $(code -b sid=$SID_P http://127.0.0.1:$F/me) |"
echo "| 정적 파일 / (회귀) | $(code http://127.0.0.1:$B/) | $(code http://127.0.0.1:$F/) |"
echo
echo "프록시 로그에 Session ID 평문: $(grep -c -E '[0-9a-f]{64}' /tmp/microhttp_step3_proxy.log)건, 백엔드 로그: $(grep -c -E '[0-9a-f]{64}' /tmp/microhttp_step3_backend.log)건"
