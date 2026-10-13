"""Report algorithm differences; require cache/Load equivalence, not baseline save equivalence."""
import argparse
import importlib.util
import json
import pathlib
import shutil
import statistics

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("run", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
source = pathlib.Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("summary", source / "docs/measurements/passenger-routing-profile-2026-10-13/Summarize-Profile.py")
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)
manifest = json.loads((a.run / "manifest.json").read_text(encoding="utf-8"))
runs = {}
for item in manifest["runs"]:
    label = item["label"]
    directory = a.run / label
    parsed = summary.parse_console((directory / f"{label}-console.txt").read_text(encoding="utf-8-sig", errors="replace"))
    process = json.loads((directory / f"{label}-process.json").read_text(encoding="utf-8-sig"))
    r = parsed["records"]
    ticks = r["Simulation tick time"]["completed_ticks"]
    if ticks != item["ticks"] or len(parsed["state_ticks"]) != 2 or parsed["state_ticks"][1] - parsed["state_ticks"][0] != ticks or process["exit_code"] != 0 or item["returncode"] or item["contamination"]:
        raise ValueError(f"Invalid ticks, process or interference: {label}")
    parsed.update(process=process, save_sha256=item["save_sha256"], derived=summary.derived(parsed), semantic_signature=summary.semantic_signature(parsed))
    parsed["derived"]["forward_ms_per_tick"] = r["Periodic departures"]["forward_us"] / ticks / 1000
    runs[label] = parsed
    for suffix in ("console.txt", "process.json"):
        shutil.copy2(directory / f"{label}-{suffix}", a.output / f"{label}-{suffix}")
    stderr = (directory / f"{label}-stderr.txt").read_text(encoding="utf-8-sig", errors="replace")
    (a.output / f"{label}-stderr-excerpt.txt").write_text("\n".join(dict.fromkeys(stderr.splitlines())) + "\n", encoding="utf-8")
speed = {}
for field in ("forward_ms_per_tick", "journey_mean_ms", "simulation_mean_ms"):
    old = [runs[f"baseline-{i}"]["derived"][field] for i in (1, 2)]
    new = [runs[f"pruned-{i}"]["derived"][field] for i in (1, 2)]
    speed[field] = dict(baseline=old, pruned=new, baseline_mean=statistics.mean(old), pruned_mean=statistics.mean(new),
                        improvement_percent=100 * (1 - statistics.mean(new) / statistics.mean(old)))
def same(a, b):
    return dict(save=a["save_sha256"] == b["save_sha256"], semantics=a["semantic_signature"] == b["semantic_signature"],
                work=a["records"]["Periodic departures"]["search_work"] == b["records"]["Periodic departures"]["search_work"],
                pruning=a["records"]["Periodic service pruning"] == b["records"]["Periodic service pruning"])
cache = same(runs["pruned-1"], runs["cold"])
repeat = same(runs["pruned-1"], runs["pruned-2"])
old, new = runs["baseline-1"]["records"], runs["pruned-1"]["records"]
changes = {
    "work": {mode: r["Periodic departures"]["search_work"] for mode, r in (("baseline", old), ("pruned", new))},
    "physical_phase_calculations_saved": old["Periodic departures"]["physical_calculations"] - new["Periodic departures"]["physical_calculations"],
    "transport_and_admission": {key: {"baseline": old.get(key), "pruned": new.get(key)} for key in ("Pragmatic routing", "Journey transport", "Schedule transport", "Untimed transport", "Active journey total", "Completed interchanges")},
    "outcomes": {mode: runs[label]["periodic_outcomes"] for mode, label in (("baseline", "baseline-1"), ("pruned", "pruned-1"))},
}
result = dict(manifest=manifest, runs=runs, speed=speed, cache_equivalence=cache, repeated_run_equivalence=repeat,
              algorithm_changes=changes, baseline_save_equal=runs["baseline-1"]["save_sha256"] == runs["pruned-1"]["save_sha256"])
old_detail = runs["baseline-detail"]["records"]["Periodic pruning profile"]
new_detail = runs["detail"]["records"]["Periodic pruning profile"]
detail_ticks = next(item["ticks"] for item in manifest["runs"] if item["label"] == "detail")
result["detail_cost"] = dict(bound_ms_per_tick=new_detail["bound_ns"] / detail_ticks / 1e6,
                            baseline_phase_ms_per_tick=old_detail["phase_ns"] / detail_ticks / 1e6,
                            pruned_phase_ms_per_tick=new_detail["phase_ns"] / detail_ticks / 1e6,
                            phase_ms_saved_per_tick=(old_detail["phase_ns"] - new_detail["phase_ns"]) / detail_ticks / 1e6,
                            instrumented_elapsed_times=True, exclusive_cpu_times=False)
(a.output / "summary.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(dict(speed=speed, cache=cache, repeat=repeat, changes=changes), indent=2))
if not all(cache.values()) or not all(repeat.values()):
    raise SystemExit("Cache or repeated-run behavior differs")
