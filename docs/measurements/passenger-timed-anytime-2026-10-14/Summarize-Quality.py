"""Archive systematic same-live-state samples. Only Certified references are optima."""
import argparse
import hashlib
import json
import math
import pathlib


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return {"count": len(values), "mean": sum(values) / len(values), "min": values[0], "max": values[-1],
            **{f"p{p}": values[math.ceil(len(values) * p / 100) - 1] for p in [50, 95, 99]}}


def summarize(rows):
    if any(r["work"] != r["old_work"] for r in rows):
        raise ValueError("Same-budget search order/Work changed")
    certified = [r for r in rows if r["found"] and r["reference_certified"]]
    if any(r["cost"] < r["reference_cost"] for r in certified):
        raise ValueError("A candidate beats a certified reference: inspect costs or reference validity")
    return {"samples": len(rows), "found": sum(r["found"] for r in rows),
            "old_found": sum(r["old_found"] for r in rows),
            "rescued": sum(r["found"] and not r["old_found"] for r in rows),
            "provisional": sum(r["state"] == 2 for r in rows),
            "certified_references": sum(r["reference_certified"] for r in rows),
            "uncertified_references": sum(r["reference_found"] and not r["reference_certified"] for r in rows),
            "reference_cost_difference": distribution([r["cost"] - r["reference_cost"] for r in certified]),
            "reference_arrival_difference": distribution([r["arrival"] - r["reference_arrival"] for r in certified]),
            "provisional_cost_difference": distribution([r["cost"] - r["reference_cost"] for r in certified if r["state"] == 2]),
            "provisional_arrival_difference": distribution([r["arrival"] - r["reference_arrival"] for r in certified if r["state"] == 2]),
            "old_certified_changed": sum(r["old_found"] and not r["old_deferred"] and
                                        (r["cost"], r["arrival"]) != (r["old_cost"], r["old_arrival"]) for r in rows),
            "notes": ["Samples are a deterministic systematic sample, not random or population-wide quality claims.",
                      "Ready, cargo, excluded service and live pools are identical for each paired query.",
                      "Reference Work is 1048576, Labels 65536 and memory 128 MiB. Uncertified references are excluded from optimum deltas.",
                      "Arrival deltas may be negative when nonzero generalized-cost penalties change the preferred arrival.",
                      "This probe adds CPU cost; its run is excluded from performance comparisons."]}


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("samples", type=pathlib.Path)
    p.add_argument("--output", type=pathlib.Path, required=True)
    a = p.parse_args()
    raw = a.samples.read_bytes()
    rows = [json.loads(line) for line in raw.decode().splitlines()]
    data = {"schema_version": 1, "raw_sha256": hashlib.sha256(raw).hexdigest(), **summarize(rows)}
    a.output.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    a.output.with_suffix(".jsonl").write_bytes(raw.replace(b"\r\n", b"\n"))
    print(json.dumps(data, ensure_ascii=False, indent=2))
