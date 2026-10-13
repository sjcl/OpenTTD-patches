"""Compare read-only pruned/unpruned queries on a single published snapshot."""
import argparse
import json
import pathlib
import re
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("console", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
a = p.parse_args()
text = a.console.read_text(encoding="utf-8-sig", errors="replace")
records = []
for line in text.splitlines():
    if line.startswith("Timed pruning quality:"):
        records.append({k: (v == "true" if v in ("true", "false") else int(v)) for k, v in re.findall(r"(\w+)=(-?\d+|true|false)", line)})
both = [r for r in records if r["old_found"] and r["new_found"]]
result = dict(queries=len(records), old_admitted=sum(r["old_found"] for r in records), new_admitted=sum(r["new_found"] for r in records),
              newly_admitted=sum(r["new_found"] and not r["old_found"] for r in records),
              lost_old=sum(r["old_found"] and not r["new_found"] for r in records), both=len(both),
              cost_changes=sum(r["old_cost"] != r["new_cost"] for r in both),
              arrival_changes=sum(r["old_arrival"] != r["new_arrival"] for r in both),
              transfer_changes=sum(r["old_transfers"] != r["new_transfers"] for r in both),
              route_changes=sum(not r["routes_equal"] for r in both),
              old_work=sum(r["old_work"] for r in records), new_work=sum(r["new_work"] for r in records),
              state_ticks=[int(x) for x in re.findall(r"^State ticks: (-?\d+)", text, re.M)], records=records)
a.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps({k: v for k, v in result.items() if k != "records"}, indent=2))
if not records or len(set(result["state_ticks"])) != 1 or result["lost_old"] or result["cost_changes"] or result["arrival_changes"] or result["transfer_changes"] or result["route_changes"]:
    raise SystemExit("Snapshot quality or stationary-state check failed")
