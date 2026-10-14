"""Validate and archive anytime evidence, including every Work histogram bucket.

Use the existing profile validator for input, tick count, process conflicts and
save hashes. Save/NewGRF/executables stay in the ignored build directory.
"""
import argparse
import importlib.util
import json
import math
import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
PROFILE = HERE.parent / "passenger-routing-profile-2026-10-13"


def load_module(name, filename):
    spec = importlib.util.spec_from_file_location(name, filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fields(line):
    return {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)(?=[;\s]|$)", line)}


def completion(text):
    lines = text.splitlines()
    summary = next((fields(line) for line in reversed(lines) if line.startswith("Timed Work completion:")), None)
    histogram = [fields(line) for line in lines if line.startswith("Timed Work histogram:")]
    # Inventory and total may both print diagnostics. Keep the last complete set.
    histogram = histogram[-65:]
    samples = [{**fields(line), "first_stop": re.search(r"first_stop=(\w+)", line)[1]}
               for line in lines if line.startswith("Timed Work sample ")][-16:]
    if summary is None or len(histogram) != 65:
        raise ValueError("Missing completion diagnostics or Work histogram")
    work_calls = summary["with_complete"] + summary["without_complete"]
    summary["rescuable_fraction"] = summary["with_complete"] / work_calls if work_calls else None
    count = sum(row["count"] for row in histogram)
    rank, cumulative, p95 = math.ceil(count * .95), 0, None
    for row in histogram:
        cumulative += row["count"]
        if cumulative >= rank:
            # These production runs reserve at most 65536 units per Timed call.
            p95 = row["upper"]
            break
    summary.update(work_histogram=histogram, work_p95_upper=p95,
                   destination_geometry=next((fields(line) for line in reversed(lines) if line.startswith("Timed Work destination geometry:")), None),
                   work_p95_overflow=p95 is None, samples=samples,
                   stops={line.split(":")[0][16:]: fields(line) for line in lines if line.startswith("Timed Work stop ")},
                   results=next((fields(line) for line in reversed(lines) if line.startswith("Timed result states:")), None),
                   adoption=next((fields(line) for line in reversed(lines) if line.startswith("Timed plan adoption:")), None))
    return summary


def validate_completion(run):
    observation = run["anytime"]
    diagnostics = run["diagnostics"]
    outcomes = diagnostics["periodic_outcomes"]
    if observation["with_complete"] + observation["without_complete"] != outcomes["Work"]["outcomes"]:
        raise ValueError("Work completion counts do not partition final Work outcomes")
    searches = diagnostics["records"]["Periodic database"]["searches"]
    if sum(row["count"] for row in observation["work_histogram"]) != searches:
        raise ValueError("Work histogram does not cover every Timed search")
    states = observation["results"]
    if states is not None:
        if sum(states[key] for key in ("unavailable", "certified", "provisional", "deferred", "no_route")) != searches:
            raise ValueError("Result states do not partition Timed searches")
        if states["certified"] != outcomes["Found"]["outcomes"]:
            raise ValueError("Found includes uncertified results")
        if states["rescued_unpopped"] > states["provisional"]:
            raise ValueError("Unpopped rescues exceed provisional results")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifests", nargs="+", type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    archive = load_module("archive", PROFILE / "Archive-Profile.py")
    # Keep the standard validation/comparison format and add our observations.
    import subprocess
    import sys
    summary_file = args.output.parent / (args.output.name + "-summary.json")
    subprocess.run([sys.executable, str(PROFILE / "Summarize-Profile.py"),
                    *map(str, args.manifests), "--output", str(summary_file)], check=True)
    data = json.loads(summary_file.read_text(encoding="utf-8"))
    for run in data["runs"]:
        manifest = next(m for m in data["manifests"] if (m["label"], m["revision"], m["kind"], m["phase"]) ==
                        (run["label"], run["revision"], run["kind"], run["phase"]) and run["id"] in {r["id"] for r in m["runs"]})
        directory = pathlib.Path(manifest["path"]).parent / run["directory"]
        run["anytime"] = completion((directory / (run["id"] + "-console.txt")).read_text(encoding="utf-8-sig"))
        validate_completion(run)
    summary_file.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    archive.archive(summary_file, args.output)
    print(args.output)


if __name__ == "__main__":
    main()
