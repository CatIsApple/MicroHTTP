#!/bin/sh
# STEP 7 실험: Session을 각 서버 메모리에 두면 서버가 2대일 때 무슨 일이 생기는가
#
#   make server && sh tests/experiments/step7_two_servers.sh
#
# A(18841), B(18842): 같은 사용자 파일을 쓰는 독립 프로세스 (Session 저장소는 각자 메모리)
# P(18840): /login, /logout, /me를 A,B로 Round-Robin 하는 프록시
set -u
cd "$(dirname "$0")/../.."
./server -p 18841 -r ./www -U tests/functional/users.txt 2>/dev/null & A=$!
./server -p 18842 -r ./www -U tests/functional/users.txt 2>/dev/null & B=$!
RR=127.0.0.1:18841,127.0.0.1:18842
./server -p 18840 -r ./www -R /login=$RR -R /logout=$RR -R /me=$RR 2>/dev/null & P=$!
trap 'kill $A $B $P 2>/dev/null; wait 2>/dev/null' EXIT
sleep 0.5
sid() { curl -s -o /dev/null -D - -d 'username=alice&password=alice-test-pw' "$1" | tr -d '\r' | sed -n 's/^Set-Cookie: sid=\([0-9a-f]*\);.*/\1/p'; }
code() { curl -s -o /dev/null -w '%{http_code}' -b "sid=$1" "$2"; }

SID=$(sid http://127.0.0.1:18841/login)
echo "## 1) A에서 로그인 → 같은 sid로 A와 B에 직접 접근"
echo "| 대상 | /me |"; echo "|---|---:|"
echo "| A (로그인한 서버) | $(code $SID http://127.0.0.1:18841/me) |"
echo "| B (다른 서버) | $(code $SID http://127.0.0.1:18842/me) |"

echo; echo "## 2) Round-Robin 프록시 뒤에서 로그인 후 /me 6번"
SID=$(sid http://127.0.0.1:18840/login)
printf '| 요청 | 1 | 2 | 3 | 4 | 5 | 6 |\n|---|---|---|---|---|---|---|\n| /me |'
for i in 1 2 3 4 5 6; do printf ' %s |' "$(code $SID http://127.0.0.1:18840/me)"; done; echo
