"""Reproduce summary/evidence packaging after the documented native runs."""
from collections import defaultdict
import csv
import gzip
import hashlib
import io
import json
from pathlib import Path
import shutil
import statistics
import subprocess

DATA = Path(__file__).resolve().parents[1]
ROOT = DATA.parents[2]
BUILD = ROOT / "build-preparation"
BASE = "79636e49744a0b0caa9f174f87b5490aa3daca9b"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write(path, value):
    temporary = path.with_suffix(path.suffix + ".partial")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    temporary.replace(path)


def main():
    assert json.loads((DATA / "validation.json").read_text())["runs"] == 64
    assert json.loads((DATA / "gui/validation.json").read_text())["runs"] == 8
    rows = json.loads((DATA / "results.json").read_text()) + json.loads((DATA / "gui/results.json").read_text())
    for mode in ("legacy", "hyperpath"):
        replay = [r for r in rows if r["mode"] == mode and r["scenario"] == "replay"]
        assert len({r["current_digest"] for r in replay}) == 1
        assert all(r["retained_ns"] > 0 for r in replay)
    groups = defaultdict(list)
    for row in rows:
        groups[(row["scenario"], row["mode"], row["requested"])].append(row)
    summary = []
    for (scenario, mode, requested), values in sorted(groups.items()):
        record = dict(scenario=scenario, mode=mode, requested=requested, repeats=len(values))
        for field in values[0]:
            if isinstance(values[0][field], int) and field not in {"digest", "current_digest", "progress_digest", "round", "requested"}:
                numbers = [r[field] for r in values]
                record[field] = {"mean": statistics.mean(numbers), "min": min(numbers), "max": max(numbers)}
        if "row_ns" in values[0] and requested:
            utilization = [r["worker_cpu_ns"] / r["row_ns"] / r["workers"] for r in values]
            record["worker_utilization"] = {"mean": statistics.mean(utilization), "min": min(utilization), "max": max(utilization)}
        summary.append(record)
    write(DATA / "summary.json", summary)
    output = io.StringIO(newline="")
    writer = csv.writer(output, lineterminator="\n")
    fields = ["wall_ns", "retained_ns", "graph_ns", "main_cpu_ns", "worker_cpu_ns", "peak_working_set", "cache_hits", "serial_rows", "worker_wait_ns", "slice_max_ns"]
    writer.writerow(["scenario", "mode", "requested", "workers", "repeats"] + [f"{field}_{stat}" for field in fields for stat in ("mean", "min", "max")])
    for row in summary:
        writer.writerow([row["scenario"], row["mode"], row["requested"], row["workers"]["mean"], row["repeats"]] +
                        [row.get(field, {}).get(stat, "") for field in fields for stat in ("mean", "min", "max")])
    (DATA / "summary.csv").write_text(output.getvalue(), encoding="utf-8", newline="\n")

    # Preserve every failed/deferred/provisional attempt, without adopting its times.
    attempts = DATA / "attempts"
    attempts.mkdir(exist_ok=True)
    for suffix in ("rejected", "deferred", "interrupted", "preliminary"):
        source = DATA.with_name(DATA.name + "-" + suffix)
        target = attempts / suffix
        assert source.resolve().is_relative_to(ROOT) and target.resolve().is_relative_to(ROOT)
        if source.exists():
            assert not target.exists()
            shutil.move(str(source), str(target))
    write(attempts / "adoption.json", {
        "accepted": ["../results.json", "../gui/results.json"],
        "rejected": "First unmonitored partial run overlapped ninja PID 28484 / clang-cl PID 138792; stopped, not adopted.",
        "deferred": "Monitor detected another worktree's native tests before the first case; no measurement adopted.",
        "interrupted": "Monitor detected ninja PID 90212 during the first serial replay; stopped, not adopted.",
        "preliminary": "64 complete runs passed digest/Work checks, but replay routing was disabled in the fixture and phase diagnostics were classified Disabled. Superseded by the final 64 runs with routing enabled.",
        "coordination": "User explicitly authorized coordination with the aircraft-cost chat; notified it before final measurement and released its hold afterward."
    })
    checks = DATA / "checks"
    for name in ("all-tests-final.log", "progress-window-test.log", "gui-benchmark-pilot.log"):
        shutil.copy2(BUILD / name, checks / name)
    assert "Passed all 783 test cases with 10514867 assertions." in (checks / "all-tests-final.log").read_text()
    assert "Passed 1 test case with 100 assertions." in (checks / "progress-window-test.log").read_text()
    for name in ("build-verified.log", "build-fixture.log", "build-gui-benchmark.log"):
        (checks / (name + ".gz")).write_bytes(gzip.compress((BUILD / name).read_bytes(), mtime=0))
    for name in ("source.json", "reference.cmake"):
        shutil.copy2(BUILD / "parallel-reference" / name, checks / ("reference-" + name))
    write(checks / "tests.json", {"native": {"exit_code": 0, "cases": 783, "assertions": 10514867, "command": ["openttd_test.exe", "--reporter", "compact"]},
        "progress_widgets": {"exit_code": 0, "cases": 1, "assertions": 100, "command": ["openttd_test.exe", "[progress-gui]", "--reporter", "compact"],
                             "OPENTTD_TEST_PROGRESS_LANGUAGE": str(BUILD / "lang/english.lng")},
        "benchmarks": {"exit_code": 0, "cases": 72, "work_and_published_digests_equal": True}})

    binary_hash = sha(BUILD / "Release/openttd_test.exe")
    assert binary_hash == json.loads((DATA / "identity.json").read_text())["sha256"]
    assert binary_hash == json.loads((DATA / "gui/identity.json").read_text())["sha256"]
    backup = Path.home() / "OpenTTD-measurements" / ("passenger-preparation-20261020-" + binary_hash[:8])
    backup.mkdir(parents=True, exist_ok=True)
    identities = []
    for source in [BUILD / "Release" / name for name in ("openttd.exe", "openttd.pdb", "openttd_test.exe", "openttd_test.pdb")] + [BUILD / "lang/english.lng"]:
        target = backup / source.name
        if not target.exists():
            shutil.copy2(source, target)
        assert sha(source) == sha(target)
        identities.append({"source": str(source), "backup": str(target), "bytes": target.stat().st_size, "sha256": sha(target), "copy": "independent file"})
    write(DATA / "binary-backup.json", identities)
    changed = subprocess.check_output(["git", "diff", BASE, "--name-only", "--", "src"], cwd=ROOT, text=True).splitlines()
    changed += subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "src"], cwd=ROOT, text=True).splitlines()
    patch = subprocess.check_output(["git", "diff", BASE, "--", "src"], cwd=ROOT)
    metadata = {"base_commit": BASE, "compiler": subprocess.check_output(["C:/Program Files/LLVM/bin/clang-cl.exe", "--version"], text=True),
        "flags": {"Release": "/O2 /Ob2 /DNDEBUG /Zi", "CRT": "MultiThreaded", "asserts": True, "LTO": False, "PGO": "OFF", "generator": "Visual Studio 17 2022 / ClangCL / x64"},
        "source_patch_sha256": hashlib.sha256(patch).hexdigest(), "source_files": {name: sha(ROOT / name) for name in sorted(set(changed))},
        "reference_generated": {name: sha(BUILD / "parallel-reference" / name) for name in ("PassengerSequentialReference.cpp", "PassengerSequentialReference.h")},
        "binaries": identities,
        "runbook_read": {"commit": "1c42420af2226a73063e4a1eb9cc5990a57229c8", "path": "docs/passenger-wpr-runbook.md", "note": "Read before preparation on the initial checkout; absent from requested jgrpp-ya base. These runs use synthetic native tests, no real-map save or ETL."}}
    # Record compiler input bytes and Git's LF form separately on this CRLF checkout.
    metadata["source_git_lf_sha256"] = {name: hashlib.sha256((ROOT / name).read_bytes().replace(b"\r\n", b"\n")).hexdigest() for name in sorted(set(changed))}
    if not subprocess.check_output(["git", "diff", "--name-only", "--", "src"], cwd=ROOT).strip() and not subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "src"], cwd=ROOT).strip():
        metadata["src_tree"] = subprocess.check_output(["git", "write-tree", "--prefix=src/"], cwd=ROOT, text=True).strip()
    write(DATA / "build-identity.json", metadata)
    write(checks / "commands.json", {"configure": ["cmake", "-S", ".", "-B", "build-preparation", "-G", "Visual Studio 17 2022", "-A", "x64",
        "-DCMAKE_TOOLCHAIN_FILE=C:/Users/bebe/.vcpkg-clion/vcpkg/scripts/buildsystems/vcpkg.cmake", "-DVCPKG_TARGET_TRIPLET=x64-windows-static", "-DVCPKG_MANIFEST_INSTALL=OFF",
        "-DVCPKG_INSTALLED_DIR=C:/Users/bebe/CLionProjects/OpenTTD-patches/cmake-build-release-visual-studio/vcpkg_installed", "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
        "-DOPTION_PGO=OFF", "-DOPTION_NO_LTO=ON", "-DOPTION_USE_ASSERTS=ON", "-DOPTION_DEDICATED=OFF", "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Zi",
        "-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Zi", "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO",
        "-DEDITBIN_EXECUTABLE=C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/editbin.exe",
        "-DCMAKE_PROJECT_INCLUDE=" + str(BUILD / "parallel-reference/reference.cmake")],
        "reference": ["python", "-B", "utils/passenger_parallel_reference_prepare.py", "build-preparation", "--reference", BASE],
        "build": ["cmake", "--build", "build-preparation", "--config", "Release", "--target", "openttd_test", "openttd", "--parallel", "12", "--", "/nodeReuse:false", "/p:MultiProcCL=true", "/p:CL_MPCount=12"],
        "measurement": ["python", "-B", "utils/passenger_preparation_benchmark.py", "build-preparation/Release/openttd_test.exe", str(DATA), "--side", "32", "--lines", "240", "--workers", "0,1,4,8", "--rounds", "2", "--pinned-reference"],
        "gui": ["python", "-B", "utils/passenger_preparation_benchmark.py", "build-preparation/Release/openttd_test.exe", str(DATA / "gui"), "--workers", "4", "--rounds", "2", "--gui-only"]})

    # Store LF consistently so staged blobs retain the manifest's byte identity.
    for path in DATA.rglob("*"):
        if path.is_file() and path.suffix in {".log", ".json", ".csv", ".py", ".cmake"}:
            contents = path.read_text(encoding="utf-8")
            if path.suffix == ".log":
                contents = contents.rstrip("\n") + "\n"
            if path.read_bytes() != contents.encode("utf-8"):
                path.write_text(contents, encoding="utf-8", newline="\n")
    write(DATA / "manifest.json", {str(path.relative_to(DATA)).replace("\\", "/"): {"bytes": path.stat().st_size, "sha256": sha(path)}
        for path in sorted(DATA.rglob("*")) if path.is_file() and path.name != "manifest.json"})
    print(json.dumps({"groups": len(summary), "runs": len(rows), "backup": str(backup)}, indent=2))


if __name__ == "__main__":
    main()
