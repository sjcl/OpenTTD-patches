"""Compare archived Legacy/precomputed evidence with one new physical trace.

Reuses passenger_transport_summary's censoring and population accounting. Old
inputs must match the committed evidence manifest; all three runs must cover
the identical input save and StateTicks interval. No missing/lost passenger is
included in the mean duration of completed trips.
"""
import argparse
import collections
import csv
import hashlib
import json
import pathlib

import passenger_transport_summary as transport


ROOT = pathlib.Path(__file__).resolve().parents[1]
BASELINE = ROOT / "docs/measurements/passenger-routing-integration-2026-10-15"
ODS = ((3222, 400), (971, 916), (3393, 3394))
PEOPLE = ((77873, 9), (215126, 1))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def baseline_proof(directory):
    manifest = json.loads((BASELINE / "manifest.json").read_text(encoding="utf-8"))
    manifest = {row["path"]: row for row in manifest}
    result = {}
    for name in ("transport-delivered.csv", "transport-pending.csv", "transport-lost.csv",
                 "transport-population.csv", "transport-services.csv", "process.json"):
        item = manifest[f"runs/{directory.name}/{name}.gz"]
        actual = sha(directory / name)
        if actual != item["original_sha256"]:
            raise ValueError(f"Archived baseline changed: {directory / name}")
        result[name] = actual
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("output", "legacy", "previous", "improved"):
        parser.add_argument(name, type=pathlib.Path)
    args = parser.parse_args()
    directories = [args.legacy, args.previous, args.improved]
    proof = {directory.name: baseline_proof(directory) for directory in directories[:2]}
    processes = [json.loads((directory / "process.json").read_text(encoding="utf-8")) for directory in directories]
    intervals = [(p["input_sha256"], p["actual-ticks.json"]["start_tick"], p["actual-ticks.json"]["end_tick"]) for p in processes]
    if len(set(intervals)) != 1:
        raise ValueError(f"Different inputs or StateTicks intervals: {intervals}")
    for index, (directory, process) in enumerate(zip(directories, processes)):
        if process["exit_code"] or process["load_failed"] or (index == 2 and process["contamination"]) or not process["save-integrity.json"]["payments_valid"]:
            raise ValueError(f"Invalid physical run: {directory}")
    runs = [transport.summarize(directory) for directory in directories]
    for directory, run in zip(directories, runs):
        if run["unaccounted_sample_starts"] or run["unaccounted_fresh_sample_starts"]:
            raise ValueError(f"Unaccounted trace samples: {directory}")
    result = dict(input_sha256=intervals[0][0], start_tick=intervals[0][1], end_tick=intervals[0][2],
                  baseline_manifest_proof=proof, baseline_reused=True,
                  archived_baseline_contamination={d.name: p["contamination"] for d, p in zip(directories[:2], processes[:2])},
                  paired_initial_people={"legacy_to_improved": transport.compare_initial(runs[0], runs[2]),
                                         "previous_to_improved": transport.compare_initial(runs[1], runs[2])},
                  target_ods={}, initial_people={}, runs=[])
    for directory, run, process in zip(directories, runs, processes):
        targets, people = {}, {}
        for origin, destination in ODS:
            od = f"{origin}:{destination}"
            targets[od] = dict(completed_fresh_ticks=run["completed_od_ticks"].get(od),
                               completed_fresh_means=run["completed_od_means"].get(od), initial=[], pending=[], lost=[])
        for state in ("delivered", "pending", "lost"):
            for row in transport.read(directory / f"transport-{state}.csv"):
                key = (row["initial_packet"], row["initial_offset"])
                if row["initial"] and key in PEOPLE:
                    people[f"{key[0]}:{key[1]}"] = dict(state=state, observed_ticks=row["at"] - row["born"], **row)
                od = f"{row['origin']}:{row['destination']}"
                if od not in targets:
                    continue
                if row["initial"]:
                    targets[od]["initial"].append(dict(state=state, **row))
                if state != "delivered":
                    targets[od][state].append(row)
        result["target_ods"][directory.name] = targets
        result["initial_people"][directory.name] = people
        run.pop("_initial")
        run["process"] = {key: value for key, value in process.items() if key not in ("actual-ticks.json", "command")}
        result["runs"].append(run)
    focused = args.improved / "transport-target-events.csv"
    if focused.is_file():
        diagnostics = {}
        with focused.open(newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                od = f"{row['origin']}:{row['destination']}"
                item = diagnostics.setdefault(od, dict(events=collections.Counter(),
                    reasons=collections.Counter(), actual_boarding_services=collections.Counter(),
                    candidate_services=collections.Counter()))
                item["events"][row["event"]] += 1
                if row["event"] == "candidate":
                    item["reasons"][row["reason"]] += 1
                    item["candidate_services"][row["candidate_service"]] += 1
                if row["event"] == "board":
                    item["actual_boarding_services"][row["actual_service"]] += 1
        result["focused_observations"] = diagnostics
    result["limitations"] = (
        "The two baselines are manifest-verified previous runs, not new reruns. "
        "The archived precomputed baseline recorded one compiler process; its timing is not reused as performance evidence. "
        "The improved run uses paused cold capture/complete rows; baseline precomputed used its saved warm snapshot. "
        "Initial samples measure remaining observed time, not the full original trip. "
        "Fresh completed OD means compare different people; pending and lost are separate. "
        "Trace CPU includes instrumentation and must not be compared as product CPU. "
        "Focused rejection logs cover queried sampled cohorts and may contain repeated cache opportunities.")
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
