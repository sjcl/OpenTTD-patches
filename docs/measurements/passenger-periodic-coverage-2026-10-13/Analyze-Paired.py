"""Retain every paired OD and its legs; count calls, never passenger people."""
import argparse
import collections
import json
import pathlib
import re
import shutil

p = argparse.ArgumentParser()
p.add_argument("console", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
a = p.parse_args()


def fields(line):
    result = {}
    for key, value in re.findall(r"(\w+)=([^;]+)", line):
        value = value.strip()
        if value in ("true", "false"):
            value = value == "true"
        elif re.fullmatch(r"-?\d+", value):
            value = int(value)
        result[key] = value
    return result


records = []
for line in a.console.read_text(encoding="utf-8", errors="replace").splitlines():
    if line.startswith("Periodic paired OD:"):
        records.append(fields(line))
        records[-1].update(legs=[], phases=[], publications=[], observations=[])
    elif records:
        for prefix, key in [("Periodic paired leg:", "legs"), ("Periodic paired phase:", "phases"),
                            ("Periodic paired publication:", "publications"), ("Periodic paired observation:", "observations")]:
            if line.startswith(prefix):
                records[-1][key].append(fields(line))

old_only = [r for r in records if r["old_found"] and not r["new_found"]]
summary = {
    "unit": "unique OD calls sampled in station pool order, not passenger counts",
    "calls": len(records),
    "old_only": len(old_only),
    "new_only": sum(not r["old_found"] and r["new_found"] for r in records),
    "both": sum(r["old_found"] and r["new_found"] for r in records),
    "neither": sum(not r["old_found"] and not r["new_found"] for r in records),
    "state_ticks": sorted({r["tick"] for r in records}),
    "old_only_outcomes": dict(collections.Counter(r["new_reason"] for r in old_only)),
    "old_only_legacy_service_compile_reasons": dict(collections.Counter(
        p["live_compile_reason"] for r in old_only for p in r["publications"])),
    "old_only_phases": dict(collections.Counter(p["live_unknown_reason"] for r in old_only for p in r["phases"])),
    "old_only_records": old_only,
    "all_records": records,
}
a.output.mkdir(parents=True, exist_ok=True)
(a.output / "paired.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
shutil.copyfile(a.console, a.output / "paired-console.txt")
print(json.dumps({k: v for k, v in summary.items() if not k.endswith("records")}, indent=2))
