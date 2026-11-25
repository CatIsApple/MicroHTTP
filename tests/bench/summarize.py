"""baseline.sh의 CSV(시나리오 x 반복)를 시나리오별 중앙값 Markdown 표로 요약합니다.

    python3 tests/bench/summarize.py results.csv
"""
import csv
import statistics
import sys

rows = list(csv.DictReader(open(sys.argv[1])))
print("| 시나리오 | 반복 | 요청/초 (중앙값) | 평균 ms | p95 ms | p99 ms | 비정상 응답 합계 |")
print("|---|---:|---:|---:|---:|---:|---:|")
for name in dict.fromkeys(r["scenario"] for r in rows):
    group = [r for r in rows if r["scenario"] == name]

    def median(key):
        return statistics.median(float(r[key]) for r in group)

    bad = sum(int(r["non_200_or_error"]) for r in group)
    print(f"| `{name}` | {len(group)} | {median('requests_per_sec'):,.0f} | {median('avg_ms'):.3f} | "
          f"{median('p95_ms'):.3f} | {median('p99_ms'):.3f} | {bad} |")
