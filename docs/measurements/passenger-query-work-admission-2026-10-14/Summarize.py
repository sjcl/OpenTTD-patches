"""Validate raw admission experiments and archive small reproducible evidence.

Save/asset/executable files remain outside Git. Archived metadata records their hashes;
run validation rehashes the actual save before claiming invariance.
"""
import argparse
import importlib.util
import json
import pathlib
import re
import shutil

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("profile", HERE.parent / "passenger-routing-profile-2026-10-13/Summarize-Profile.py")
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


def check_accounting(run):
    records = run["diagnostics"]["records"]
    a, w = records.get("Periodic admission", {}), records.get("Periodic allocation", {})
    if a.get("accepted", 0):
        if a["reserved"] != a["used"] + a["returned"]:
            raise ValueError("Work reservation did not settle")
        if a["used"] != w["timed_used"] + w["schedule_used"]:
            raise ValueError("Planner work does not equal admission usage")
        if a["max_tick_work"] > 1048576:
            raise ValueError("Global tick Work exceeded")
        if a["used"] != records["Graph work"]["query_allowance"]:
            raise ValueError("Query counter did not charge actual work")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("manifests", nargs="+", type=pathlib.Path)
    p.add_argument("--output", type=pathlib.Path, required=True)
    p.add_argument("--archive", type=pathlib.Path)
    a = p.parse_args()
    datasets, all_runs = [], []
    for path in a.manifests:
        manifest, runs = profile.read_manifest(path.resolve())
        if manifest["status"] != "complete":
            raise ValueError("Measurement manifest is incomplete")
        for run in runs:
            if not run["valid"]:
                raise ValueError(str(run["errors"]))
            check_accounting(run)
            records = run["diagnostics"]["records"]
            run["fingerprint"] = profile.fingerprint(manifest, run)
            run["metrics"] = {key: records.get(key) for key in ["Periodic admission", "Periodic allocation", "Periodic Work outcomes",
                "Periodic missing", "Work exhausted OD transport", "Timed result states", "Timed Work completion", "Schedule routing",
                "Journey transport", "Schedule transport", "Untimed transport", "Completed interchanges", "Passenger inventory",
                "Waiting routing", "Waiting age sample", "Schedule inventory", "Inventory Legacy", "Inventory Journey", "Inventory Fallback"]}
            # A current census includes newly created ODs; it is not a failure cohort.
            run["metrics"]["known_od_not_yet_delivered"] = sum(records.get(key, {}).get("known_OD", 0)
                for key in ["Inventory Legacy", "Inventory Journey", "Inventory Fallback"])
            run["metrics"]["total_delivered"] = sum(records.get(key, {}).get("delivered", 0)
                for key in ["Journey transport", "Schedule transport", "Untimed transport"])
            text = (path.parent / run["directory"] / (run["id"] + "-console.txt")).read_text(encoding="utf-8-sig")
            run["admission_tick_queries"] = {int(k): int(v) for k, v in re.findall(r"^Periodic tick searches (\d+): (\d+)", text, re.M)}
            run["admission_tick_work"] = {int(k): int(v) for k, v in re.findall(r"^Periodic tick Work upper (\d+): (\d+)", text, re.M)}
            if run["metrics"]["Periodic admission"].get("accepted", 0):
                if sum(run["admission_tick_queries"].values()) != manifest["ticks"]:
                    raise ValueError("Admission histogram tick count mismatch")
                if sum(k * v for k, v in run["admission_tick_queries"].items()) != run["metrics"]["Periodic admission"]["accepted"]:
                    raise ValueError("Admission histogram query count mismatch")
            if a.archive:
                target = a.archive / (path.parent.parent.name + "-" + manifest["label"] + "-" + run["id"])
                target.mkdir(parents=True, exist_ok=True)
                directory = path.parent / run["directory"]
                shutil.copy2(directory / (run["id"] + "-console.txt"), target / "console.txt")
                shutil.copy2(directory / (run["id"] + "-process.json"), target / "process.json")
                (target / "validation.json").write_text(json.dumps(run["evidence"], indent=2) + "\n", encoding="utf-8")
            all_runs.append(run)
        datasets.append({"manifest": str(path.resolve()), "metadata": manifest})
    comparisons = []
    controls = [r for r in all_runs if r["label"] == "control"]
    for run in all_runs:
        if not controls or run["phase"] != controls[0]["phase"]:
            continue
        base = controls[0]
        comparisons.append({"label": run["label"], "id": run["id"], "mode": run["mode"],
            "compatible": run["fingerprint"] == base["fingerprint"],
            "simulation_mean_ratio": run["derived"]["simulation_mean_ms"] / base["derived"]["simulation_mean_ms"],
            "total_delivery_ratio": run["metrics"]["total_delivered"] / base["metrics"]["total_delivered"],
            "timed_delivery_ratio": run["metrics"]["Journey transport"]["delivered"] / base["metrics"]["Journey transport"]["delivered"]})
    result = {"datasets": datasets, "runs": all_runs, "comparisons": comparisons,
              "notes": ["Times are matched observations, not a causal estimate or confidence interval.",
                        "Process CPU and peaks include load/save and workers; quantiles are 1ms upper bounds.",
                        "Work stop and failure to obtain either route have different counters.",
                        "Transit age includes travel; intent waiting resets on replan."]}
    a.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    for run in all_runs:
        m, d = run["metrics"], run["derived"]
        print(run["label"], run["id"], f"mean={d['simulation_mean_ms']:.3f}ms",
              f"delivery={m['total_delivered']}", f"timed={m['Journey transport']['delivered']}",
              f"admission={m['Periodic admission']}")


if __name__ == "__main__":
    main()
