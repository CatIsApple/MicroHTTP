"""hey -o csv 출력(요청 1건당 1줄)에서 처리량·지연 통계를 계산합니다.

    hey -n 1000 -c 10 -o csv URL | python3 tests/bench/hey_stats.py <시나리오> <실행번호> <요청수>

hey 요약 화면은 시간을 0.1ms 단위로만 보여 주므로, 요청별 원시 시간(초, 소수 4자리 이상)으로 직접 계산합니다.
- requests_per_sec : 응답 받은 요청 수 / (가장 늦게 끝난 요청의 offset + response-time)
- p95, p99         : nearest-rank 백분위
- non_200_or_error : 200이 아닌 응답 + 응답을 받지 못한 요청(요청수 - CSV 줄 수)
"""
import csv
import math
import sys

name, run, expected = sys.argv[1], sys.argv[2], int(sys.argv[3])
rows = list(csv.DictReader(sys.stdin))
times = sorted(float(r["response-time"]) for r in rows)
ends = [float(r["offset"]) + float(r["response-time"]) for r in rows]
ok = sum(1 for r in rows if r["status-code"] == "200")


def percentile(p):
    return times[max(0, math.ceil(p / 100 * len(times)) - 1)] * 1000 if times else float("nan")


rps = len(rows) / max(ends) if rows else 0.0
avg = sum(times) / len(times) * 1000 if times else float("nan")
print(f"{name},{run},{rps:.1f},{avg:.3f},{percentile(95):.3f},{percentile(99):.3f},{ok},{expected - ok}")
