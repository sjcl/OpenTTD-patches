"""Summarize paired allocation samples and unobserved query latencies separately."""
import argparse
import csv
import json
import math
import pathlib
import statistics

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("log", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
a = p.parse_args()
lines = a.log.read_text(encoding="utf-8-sig", errors="replace").splitlines()
rows = list(csv.DictReader(line[len("ARENA_CSV,"):] for line in lines if line.startswith("ARENA_CSV,")))
for row in rows:
    for key in row.keys() - {"fixture", "cache", "algorithm"}:
        row[key] = int(row[key])

def quantile(values, percentile):
    return sorted(values)[max(0, math.ceil(len(values) * percentile / 100) - 1)]

summary = {}
for fixture in sorted({r["fixture"] for r in rows}):
    summary[fixture] = {}
    for cache in ("cold", "warm", "disabled"):
        result = {}
        for algorithm in ("legacy", "arena"):
            samples = [r for r in rows if r["fixture"] == fixture and r["cache"] == cache and r["algorithm"] == algorithm]
            plain = [r for r in rows if r["fixture"] == fixture and r["cache"] == cache and r["algorithm"] == algorithm + "-untracked"]
            assert len(samples) == len(plain) == 30
            assert max(r["peak_new_bytes"] for r in samples) < 2**63, "Allocation accounting underflow"
            times = [r["ns"] / 1000 for r in plain]
            result[algorithm] = dict(samples=len(plain), mean_us=statistics.mean(times), median_us=statistics.median(times),
                                     p95_us=quantile(times, 95), p99_us=quantile(times, 99),
                                     forward_mean_us=statistics.mean(r["forward_us"] for r in plain),
                                     allocations_median=statistics.median(r["allocations"] for r in samples),
                                     allocated_bytes_median=statistics.median(r["allocated_bytes"] for r in samples),
                                     physical_peak_bytes=max(r["peak_new_bytes"] + r["scratch_before"] for r in samples),
                                     retained_peak_bytes=max(r["scratch_retained"] for r in samples),
                                     labels_max=max(r["labels"] for r in samples), frontier_lookups_max=max(r["frontier_lookups"] for r in samples))
        for run in range(30):
            pair = [r for r in rows if r["fixture"] == fixture and r["cache"] == cache and r["run"] == run]
            for field in ("labels", "frontier_lookups", "queue_pushes", "queue_pops", "work", "memory"):
                assert len({r[field] for r in pair}) == 1, f"Semantic change: {fixture}/{cache}/{run}/{field}"
        result["mean_improvement_percent"] = 100 * (1 - result["arena"]["mean_us"] / result["legacy"]["mean_us"])
        summary[fixture][cache] = result
a.output.write_text(json.dumps(dict(layout=[line for line in lines if line.startswith("ARENA_LAYOUT,")], summary=summary), indent=2) + "\n", encoding="utf-8")
print(json.dumps(summary, indent=2))
