"""Parse raw console evidence and compare compatible A/B/baseline manifests.

Missing observations stay null. Detail times never enter performance comparisons.
Histograms use nearest-rank quantiles with 1 ms upper bounds, not exact samples.
"""
import argparse
import hashlib
import json
import math
import pathlib
import re
import statistics


def number(value):
    if value in ("true", "false"):
        return value == "true"
    return float(value) if "." in value else int(value)


def parse_console(text):
    records, buckets, outcomes, fallbacks, observations = {}, [], {}, {}, {}
    for line in text.splitlines():
        fields = {key: number(value) for key, value in re.findall(r"(\w+)=(-?\d+(?:\.\d+)?|true|false)(?=[;\s]|$)", line)}
        if line.startswith("Profile tick bucket:"):
            buckets.append(fields)
        elif line.startswith("Periodic OD calls:"):
            outcomes[line.split(":", 1)[1].strip().split()[0]] = fields
        elif line.startswith("Fallback "):
            reason, rest = line.split(":", 1)
            match = re.search(r"(\d+) packets; deferred=(\d+)", rest)
            if match:
                fallbacks[reason[9:]] = {"fallback_packets": int(match[1]), "deferred_packets": int(match[2])}
        elif re.match(r'(Graph failure|Trip stop|Schedule invalidation) \w+: \d+', line):
            match = re.match(r'(Graph failure|Trip stop|Schedule invalidation) (\w+): (\d+)', line)
            observations.setdefault(match[1], {})[match[2]] = int(match[3])
        elif line.startswith('Periodic phase work stops:'):
            observations['phase_capture_work'] = int(re.search(r': (\d+)', line)[1])
        elif any(line.startswith(prefix) for prefix in ['Graph work=', 'Train observations=', 'Peak graph retained_estimate=']):
            prefix = next(p for p in ['Graph work=', 'Train observations=', 'Peak graph retained_estimate='] if line.startswith(p))
            records[prefix.split('=')[0]] = fields
        elif ":" in line and fields:
            prefix = line.split(":", 1)[0]
            records[prefix] = fields
            if prefix.startswith("Active journey"):
                match = re.search(r"applied=(\d+) packets/(\d+) passengers", line)
                if match:
                    fields.update(applied_packets=int(match[1]), applied_passengers=int(match[2]))
    return {"state_ticks": [int(t) for t in re.findall(r"^State ticks: (-?\d+)", text, re.M)],
            "records": records, "tick_histogram": buckets, "periodic_outcomes": outcomes,
            "fallbacks": fallbacks, "observations": observations}


def quantile(buckets, key, percentile, maximum):
    count = sum(b.get(key, 0) for b in buckets)
    if not count:
        return None
    rank, cumulative = math.ceil(count * percentile / 100), 0
    for bucket in sorted(buckets, key=lambda b: b["index"]):
        cumulative += bucket.get(key, 0)
        if cumulative >= rank:
            # The last bucket is unbounded. Never call 2048 ms an overflow bound.
            upper = (bucket["index"] + 1) * 1000 if bucket["index"] < 2047 else None
            if upper is not None and maximum is not None:
                upper = min(upper, maximum)
            return {"upper_us": upper, "bucket_index": bucket["index"], "overflow": bucket["index"] == 2047}
    raise ValueError("Invalid histogram")


def ratio(numerator, denominator):
    return numerator / denominator if numerator is not None and denominator else None


def derived(parsed):
    r = parsed["records"]
    sim = r.get("Simulation tick time", {})
    hist = r.get("Tick histogram (1ms upper bounds)", {})
    ticks = sim.get("completed_ticks", 0)
    db, cache = r.get("Periodic database", {}), r.get("Periodic cache cost", {})
    schedule = r.get("Schedule routing", {})
    ms = lambda value: ratio(value, ticks * 1000)
    query, reverse = db.get("query_us"), cache.get("compile_us")
    buckets = parsed["tick_histogram"]
    result = {"completed_ticks": ticks,
              "simulation_mean_ms": ms(sim.get("total_us")), "simulation_max_ms": ratio(sim.get("max_us"), 1000),
              "journey_mean_ms": ratio(hist.get("journey_mean_us"), 1000),
              "journey_max_ms": ratio(hist.get("journey_max_us"), 1000),
              "quantile_resolution_us": 1000,
              "quantiles": {key: {f"p{p}": quantile(buckets, key, p, sim.get("max_us") if key == "simulation" else hist.get("journey_max_us"))
                                  for p in [50, 95, 99]} for key in ["simulation", "journey"]},
              "time_ms_per_tick": {
                  "timed_query_inclusive": ms(query), "timed_reverse_cold": ms(reverse),
                  "timed_forward_and_cache_other": ms(query - reverse) if query is not None and reverse is not None else None,
                  "schedule_query_inclusive": ms(schedule.get("query_us")),
                  "schedule_compile": ms(r.get("Schedule policy compilation", {}).get("compile_us")),
                  "database_tick_inclusive": ms(db.get("build_us")),
                  "database_publication": ms(r.get("Periodic publication cost", {}).get("publication_us")),
                  **{key: ms(value) for key, value in r.get("Journey inclusive scopes", {}).items()},
              },
              "calls": {"timed_od": db.get("searches"), "next_departures": db.get("next_departures"),
                        "schedule_od": schedule.get("hits", 0) + schedule.get("misses", 0) if schedule else None,
                        "fallback_cold_searches": r.get("OD fallback cache", {}).get("searches")},
              "cache_hit_fraction": {"timed": ratio(db.get("policy_hits"), db.get("policy_hits", 0) + db.get("policy_misses", 0)),
                                     "schedule": ratio(schedule.get("hits"), schedule.get("hits", 0) + schedule.get("misses", 0))}}
    if not buckets:
        # Historical logs have no p50: keep it unknown, retain their reported p95/p99.
        for key in ["simulation", "journey"]:
            for p in [95, 99]:
                value = hist.get(f"{key}_p{p}_us")
                if value is not None:
                    result["quantiles"][key][f"p{p}"] = {"upper_us": value, "overflow": None, "bucket_index": None}
    return result


def semantic_signature(parsed):
    r = parsed["records"]
    db, schedule = r.get("Periodic database", {}), r.get("Schedule routing", {})
    keys = ["Journey transport", "Timed depot transport", "Schedule transport", "Untimed transport",
            "Pragmatic routing", "Completed interchanges", "Schedule-assisted final OD", "OD fallback", "Fair waiting shares"]
    return {"ticks": parsed["state_ticks"], "outcomes": parsed["periodic_outcomes"], "fallbacks": parsed["fallbacks"],
            "observations": parsed.get('observations', {}),
            "transport_and_admission": {key: r.get(key) for key in keys},
            "database": {key: db.get(key) for key in ["builds", "work", "searches", "next_departures", "rejected_work", "rejected_memory"]},
            "schedule": {key: schedule.get(key) for key in ["packets", "passengers", "timed_deferred", "work"]},
            "schedule_searches": schedule.get("hits", 0) + schedule.get("misses", 0) if schedule else None}


def read_manifest(path):
    manifest = json.loads(path.read_text(encoding="utf-8"))
    runs = []
    for record in manifest["runs"]:
        directory = path.parent / record["directory"]
        console_path = directory / f"{record['id']}-console.txt"
        console = console_path.read_text(encoding="utf-8-sig", errors="replace")
        parsed = parse_console(console)
        process = json.loads((directory / f"{record['id']}-process.json").read_text(encoding="utf-8-sig"))
        stats = derived(parsed)
        errors = []
        interval = parsed["state_ticks"]
        if len(interval) != 2 or interval[1] - interval[0] != manifest["ticks"]:
            errors.append("StateTicks interval does not match requested simulation ticks")
        if stats["completed_ticks"] != manifest["ticks"]:
            errors.append("Completed simulation ticks do not match requested driver loops")
        if parsed["tick_histogram"]:
            for key in ["simulation", "journey"]:
                if sum(b[key] for b in parsed["tick_histogram"]) != stats["completed_ticks"]:
                    errors.append(key + " histogram sample count mismatch")
        if process.get("exit_code") != 0 or record["returncode"] != 0:
            errors.append("Process did not exit successfully")
        if process.get("input_sha256", "").lower() != manifest["package"]["input_sha256"]:
            errors.append("Input save SHA-256 mismatch")
        if process.get("binary_sha256", "").lower() != manifest["binary_sha256"]:
            errors.append("Binary SHA-256 mismatch")
        if record.get("contamination"):
            errors.append("Competing process detected during measurement")
        save = directory / f"{record['id']}-result.sav"
        final_hash = None
        if save.exists():
            with save.open("rb") as stream:
                final_hash = hashlib.file_digest(stream, "sha256").hexdigest()
        if final_hash is None or final_hash != record["final_save_sha256"]:
            errors.append("Final save missing or changed")
        stderr = (directory / f"{record['id']}-stderr.txt").read_bytes()
        warnings = list(dict.fromkeys(stderr.decode("utf-8", errors="replace").splitlines()))
        run = {**record, "label": manifest["label"], "kind": manifest["kind"], "phase": manifest["phase"],
               "revision": manifest["revision"], "valid": not errors, "errors": errors,
               "process": process, "diagnostics": parsed, "derived": stats,
               "semantic_signature": semantic_signature(parsed),
               "evidence": {"console_sha256": hashlib.sha256(console_path.read_bytes()).hexdigest(),
                            "stderr_sha256": hashlib.sha256(stderr).hexdigest(), "stderr_bytes": len(stderr), "unique_stderr_lines": warnings}}
        detail = directory / "detail-scopes.json"
        if detail.exists():
            run["detail"] = json.loads(detail.read_text(encoding="utf-8"))
        if manifest["kind"] == "detail" and "detail" not in run:
            errors.append("Detailed scopes JSON missing")
            run["valid"] = False
        runs.append(run)
    return manifest, runs


def fingerprint(manifest, run):
    compiler = {key: value for key, value in manifest["compiler"].items() if key not in ("CMAKE_PREFIX_PATH", "CMAKE_CXX_COMPILER")}
    return {"input": manifest["package"]["input_sha256"], "assets": manifest["package"]["asset_manifest_sha256"],
            "config": manifest["package"]["config_sha256"], "compiler": compiler, "host": manifest["host"],
            "settings": manifest["settings"], "ticks": run["diagnostics"]["state_ticks"], "kind": manifest["kind"],
            "harness": manifest["harness_sha256"], "phase": manifest["phase"], "cache_off_scope": manifest["cache_off_scope"]}


def summarize(paths, baseline):
    manifests, all_runs, fingerprints = [], [], {}
    for path in paths:
        manifest, runs = read_manifest(path)
        manifests.append({"path": str(path), **manifest})
        for run in runs:
            identity = (run["label"], run["id"], run["kind"], run["phase"])
            if identity in fingerprints:
                raise ValueError(f"Duplicate run identity {identity}; use distinct labels for separate datasets")
            fingerprints[identity] = fingerprint(manifest, run)
        all_runs.extend(runs)
    groups, invariance, comparisons = [], [], []
    identities = sorted(set((r["label"], r["kind"], r["phase"]) for r in all_runs))
    for label, kind, phase in identities:
        runs = [r for r in all_runs if (r["label"], r["kind"], r["phase"]) == (label, kind, phase) and r["valid"]]
        for mode in sorted(set(r["mode"] for r in runs)):
            subset = [r for r in runs if r["mode"] == mode]
            means = [r["derived"]["simulation_mean_ms"] for r in subset]
            groups.append({"label": label, "kind": kind, "phase": phase, "mode": mode, "runs": len(subset),
                           "mean_of_run_means_ms": statistics.mean(means), "median_of_run_means_ms": statistics.median(means),
                           "run_mean_range_ms": [min(means), max(means)],
                           "run_mean_relative_range": ratio(max(means) - min(means), statistics.mean(means)),
                           "repeat_semantics_identical": len({json.dumps(r["semantic_signature"], sort_keys=True) for r in subset}) == 1,
                           "repeat_save_hash_identical": len({r["final_save_sha256"] for r in subset}) == 1})
        enabled, disabled = [r for r in runs if r["mode"] == "on"], [r for r in runs if r["mode"] == "cold"]
        compared = enabled + disabled
        invariance.append({"label": label, "kind": kind, "phase": phase,
                           "status": "measured" if enabled and disabled else "not_measured",
                           "full_save_identical": len({r["final_save_sha256"] for r in compared}) == 1 if enabled and disabled else None,
                           "work_transport_outcomes_identical": len({json.dumps(r["semantic_signature"], sort_keys=True) for r in compared}) == 1 if enabled and disabled else None})
    for group in groups:
        if group["label"] == baseline or group["kind"] != "minimal":
            continue
        reference = next((g for g in groups if g["label"] == baseline and (g["mode"], g["kind"], g["phase"]) == (group["mode"], group["kind"], group["phase"])), None)
        if not reference:
            comparisons.append({"label": group["label"], "mode": group["mode"], "compatible": False, "reason": "matching minimal baseline unavailable"})
            continue
        candidate_runs = [r for r in all_runs if r["valid"] and (r["label"], r["kind"], r["phase"], r["mode"]) == (group["label"], "minimal", group["phase"], group["mode"])]
        reference_runs = [r for r in all_runs if r["valid"] and (r["label"], r["kind"], r["phase"], r["mode"]) == (baseline, "minimal", group["phase"], group["mode"])]
        values = [fingerprints[(r["label"], r["id"], r["kind"], r["phase"])] for r in candidate_runs + reference_runs]
        compatible = all(value == values[0] for value in values)
        comparisons.append({"label": group["label"], "mode": group["mode"], "phase": group["phase"], "compatible": compatible,
                            "relative_change": group["mean_of_run_means_ms"] / reference["mean_of_run_means_ms"] - 1 if compatible else None,
                            "semantics_match_baseline": all(r["semantic_signature"] == reference_runs[0]["semantic_signature"] for r in candidate_runs) if compatible else None,
                            "reason": None if compatible else "input/assets/config/compiler/host/settings/interval/harness differ"})
    return {"schema_version": 1, "baseline_label": baseline, "manifests": manifests, "runs": all_runs,
            "groups": groups, "cache_invariance": invariance, "comparisons": comparisons,
            "scope_notes": ["Elapsed scopes are inclusive and overlap; never sum waiting/boarding/fallback with query times.",
                            "Database build_us spans Tick and includes Publish; publication_us is its child and is not added again.",
                            "Histograms have 1 ms upper bounds and an unbounded overflow bucket.",
                            "Detail runs are excluded from speed comparisons; process CPU includes load/save and worker threads.",
                            "Run means compare matched intervals; repeat ranges are environment noise observations, not confidence intervals."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifests", nargs="+", type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--baseline", default="baseline")
    args = parser.parse_args()
    result = summarize(args.manifests, args.baseline)
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"groups": result["groups"], "cache_invariance": result["cache_invariance"], "comparisons": result["comparisons"]}, indent=2))
