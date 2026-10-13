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
    departures = r.get('Periodic departures', {})
    storage = r.get('Timed search storage', {})
    binding = r.get('OD fallback bindings', {})
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
                  "timed_forward": ms(departures.get('forward_us')),
                  "departure_index_build": ratio(departures.get('build_ns'), ticks * 1000000),
                  "fallback_binding_validation": ratio(binding.get('validation_ns'), ticks * 1000000),
                  "fallback_binding_build": ratio(binding.get('build_ns'), ticks * 1000000),
                  "fallback_reverse_build": ratio(binding.get('reverse_build_ns'), ticks * 1000000),
                  "schedule_query_inclusive": ms(schedule.get("query_us")),
                  "schedule_compile": ms(r.get("Schedule policy compilation", {}).get("compile_us")),
                  "database_tick_inclusive": ms(db.get("build_us")),
                  "database_publication": ms(r.get("Periodic publication cost", {}).get("publication_us")),
                  **{key: ms(value) for key, value in r.get("Journey inclusive scopes", {}).items()},
              },
              "calls": {"timed_od": db.get("searches"), "next_departures": db.get("next_departures"),
                        "logical_vehicle_candidates": departures.get('logical_vehicles'),
                        "physical_departure_calculations": departures.get('physical_calculations'),
                        "pruned_services": r.get('Periodic service pruning', {}).get('pruned_services'),
                        "labels_accepted": storage.get('labels'), "frontier_lookups": storage.get('frontier_lookups'),
                        "queue_pushes": storage.get('queue_pushes'), "queue_pops": storage.get('queue_pops'),
                        "schedule_od": schedule.get("hits", 0) + schedule.get("misses", 0) if schedule else None,
                        "fallback_cold_searches": r.get("OD fallback cache", {}).get("searches")},
              "memory_bytes": {"database_estimate": db.get('bytes'), "policy_and_residency_metadata_estimate": db.get('policies_bytes'),
                               "departure_index_estimate": departures.get('bytes'), "departure_index_peak_estimate": departures.get('peak_bytes'),
                               "search_scratch_retained": storage.get('scratch_bytes'), "search_scratch_peak": storage.get('scratch_peak_bytes')},
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
            "transport_and_admission": {key: ({'work': r[key].get('work')} if key == 'OD fallback' and key in r else r.get(key)) for key in keys},
            "database": {key: db.get(key) for key in ["builds", "work", "searches", "next_departures", "rejected_work", "rejected_memory"]},
            "timed_search_work": r.get('Periodic departures', {}).get('search_work'),
            "schedule": {key: schedule.get(key) for key in ["packets", "passengers", "timed_deferred", "work"]},
            "schedule_searches": schedule.get("hits", 0) + schedule.get("misses", 0) if schedule else None}


def detail_windows(detail):
    """Observed consecutive intervals; never assume a steady-state boundary."""
    samples, checkpoints = detail.get('tick_samples', []), detail.get('checkpoints', [])
    windows, previous, start = [], {}, 0
    for checkpoint in checkpoints:
        end, current = checkpoint['completed_ticks'], checkpoint['metrics']
        if end <= start or end > len(samples):
            raise ValueError('Detail checkpoint is outside recorded simulation ticks')
        subset = samples[start:end]
        metrics = {}
        for name in current.keys() | previous.keys():
            now, old = current.get(name, {}), previous.get(name, {})
            calls = now.get('calls', 0) - old.get('calls', 0)
            ns = now.get('inclusive_ns', 0) - old.get('inclusive_ns', 0)
            if calls < 0 or ns < 0:
                raise ValueError('Detail cumulative counter decreased')
            metrics[name] = {'calls': calls, 'inclusive_ns': ns, 'inclusive_ms_per_tick': ns / (end - start) / 1000000}
            if 'exclusive_ns' in now:
                exclusive = now['exclusive_ns'] - old.get('exclusive_ns', 0)
                if exclusive < 0 or exclusive > ns:
                    raise ValueError('Detail exclusive counter is invalid')
                metrics[name].update(exclusive_ns=exclusive, exclusive_ms_per_tick=exclusive / (end - start) / 1000000)
        def latency(column):
            values = sorted(row[column] for row in subset)
            return {'mean_us': statistics.mean(values), 'max_us': max(values),
                    **{f'p{p}_us': values[math.ceil(len(values) * p / 100) - 1] for p in [50, 95, 99]}}
        windows.append({'first_ordinal': start + 1, 'last_ordinal': end, 'kind': 'detail',
                        'simulation': latency(0), 'journey': latency(1), 'metrics': metrics})
        start, previous = end, current
    return windows


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
            run['detail_windows'] = detail_windows(run['detail'])
            if run['detail'].get('tick_samples') and len(run['detail']['tick_samples']) != stats['completed_ticks']:
                errors.append('Detailed tick sample count mismatch')
                run['valid'] = False
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


def snapshot_datasets(path):
    """Reuse recorded validation; do not pretend to recheck absent large saves."""
    snapshot = json.loads(path.read_text(encoding='utf-8'))
    if snapshot.get('schema_version') != 1:
        raise ValueError('Unsupported archived summary schema')
    assigned = set()
    for manifest in snapshot['manifests']:
        ids = {r['id'] for r in manifest['runs']}
        runs = []
        for index, run in enumerate(snapshot['runs']):
            if (run['label'], run['kind'], run['phase'], run['revision']) == (manifest['label'], manifest['kind'], manifest['phase'], manifest['revision']) and run['id'] in ids:
                if index in assigned:
                    raise ValueError('Archived run matches multiple manifests')
                assigned.add(index)
                runs.append(run)
        yield manifest, runs
    if len(assigned) != len(snapshot['runs']):
        raise ValueError('Archived run lacks matching manifest provenance')


def summarize(paths, baseline, archived_summaries=()):
    manifests, all_runs, fingerprints = [], [], {}
    datasets = []
    for path in archived_summaries:
        datasets.extend(snapshot_datasets(path))
    for path in paths:
        manifest, runs = read_manifest(path)
        datasets.append(({"path": str(path), **manifest}, runs))
    for manifest, runs in datasets:
        manifests.append(manifest)
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
                            "run_mean_relative_ranges": {"baseline": reference["run_mean_relative_range"], "candidate": group["run_mean_relative_range"]},
                            "semantics_match_baseline": all(r["semantic_signature"] == reference_runs[0]["semantic_signature"] for r in candidate_runs) if compatible else None,
                            "full_save_match_baseline": all(r["final_save_sha256"] == reference_runs[0]["final_save_sha256"] for r in candidate_runs + reference_runs) if compatible else None,
                            "reason": None if compatible else "input/assets/config/compiler/host/settings/interval/harness differ"})
    return {"schema_version": 1, "baseline_label": baseline, "manifests": manifests, "runs": all_runs,
            "imported_summary_paths": [str(p) for p in archived_summaries],
            "groups": groups, "cache_invariance": invariance, "comparisons": comparisons,
            "scope_notes": ["Elapsed scopes are inclusive and overlap; never sum waiting/boarding/fallback with query times.",
                            "Database build_us spans Tick and includes Publish; publication_us is its child and is not added again.",
                            "Histograms have 1 ms upper bounds and an unbounded overflow bucket.",
                            "Detail runs are excluded from speed comparisons; process CPU includes load/save and worker threads.",
                            "Detail exclusive_ns subtracts observed nested scopes only; it is elapsed time, not an OS exclusive CPU sample.",
                            "Run means compare matched intervals; repeat ranges are environment noise observations, not confidence intervals.",
                            "Compatible fingerprints do not establish stable CPU clocks or environmental conditions; relative_change is an observation, not causal attribution to a patch.",
                            "Imported summaries retain validation recorded at collection; they do not rehash absent full saves."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifests", nargs="*", type=pathlib.Path)
    parser.add_argument('--existing-summary', action='append', default=[], type=pathlib.Path, help='Reuse archived baseline diagnostics and their recorded validation; full saves are not rechecked')
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--baseline", default="baseline")
    args = parser.parse_args()
    if not args.manifests and not args.existing_summary:
        parser.error('Provide manifests or --existing-summary')
    result = summarize(args.manifests, args.baseline, args.existing_summary)
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"groups": result["groups"], "cache_invariance": result["cache_invariance"], "comparisons": result["comparisons"]}, indent=2))
