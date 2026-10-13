"""Archive small text evidence and exact binary/source identifiers; never archive assets or saves."""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import shutil
import subprocess
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("build", type=pathlib.Path)
p.add_argument("--network", default="mp-pruning-unrestricted")
a = p.parse_args()
build = a.build.resolve()
source = pathlib.Path(__file__).resolve().parents[3]
output = pathlib.Path(__file__).resolve().parent
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
for src, dst in [("passenger-quality-final.log", "passenger-tests.log"), ("ctest-final-product.log", "ctest-product.log"),
                 ("mp-unrestricted-harness.log", "mp-harness.log")]:
    shutil.copy2(build / src, output / dst)
network = build / a.network
result = json.loads((network / "result.json").read_text(encoding="utf-8"))
if result["detected_sync_failure"] or result["connected_clients_at_end"] != 2:
    raise ValueError("Cannot archive an unsuccessful join test as successful")
shutil.copy2(network / "result.json", output / "mp-result.json")
for name in ("server", "client-initial", "client-late"):
    directory = network / name
    for filename in ("console.txt", "progress-console.txt"):
        if (directory / filename).exists():
            shutil.copy2(directory / filename, output / f"mp-{name}-{filename}")
    stderr = (directory / "stderr.txt").read_text(encoding="utf-8-sig", errors="replace")
    (output / f"mp-{name}-stderr-excerpt.txt").write_text("\n".join(dict.fromkeys(stderr.splitlines())) + "\n", encoding="utf-8")
shutil.copy2(build / "mp-pruning/server/quality-console.txt", output / "quality-console.txt")
spec = importlib.util.spec_from_file_location("runner", source / "docs/measurements/passenger-routing-profile-2026-10-13/Run-Profile.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
metadata = dict(base_commit="56b0f287b013d4775cbd62c4ec5d5801c03f8e40", branch="perf/passenger-timed-pruning",
                compiler=runner.compiler_info(build), performance_benchmark_option="ON", product_benchmark_option="OFF",
                host_cpu=runner.powershell("Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors | ConvertTo-Json"),
                binary_sha256={f.name: sha(f) for f in build.glob("openttd-*.exe")},
                source_sha256={name: sha(source / name) for name in ("src/passenger_service_database.cpp", "src/passenger_service_database.h", "src/console_cmds.cpp", "src/tests/passenger_service_database.cpp")},
                network_probe_sha256=sha(build / "network-probe/null_v.cpp"),
                input_sha256=sha(build / "package/input.sav"), package=json.loads((build / "package/package.json").read_text(encoding="utf-8")),
                passenger_cases=343, passenger_assertions=1327171, product_ctest_passed=556, product_ctest_total=556,
                performance_runs=7, performance_pair_ticks=2048, detail_ticks=512,
                routing_changes_after_performance="None; only strengthen the same-service reboarding test and prepare copied diagnostic harnesses.",
                network_code_changes="Copied null driver adds optional read-only progress diagnostics every 128 state ticks; identical routing code; GUI subsystem changed to console.",
                failed_attempts=["PowerShell 5 hash-recording failure after completion; excluded and all measurements restarted with PowerShell 7.",
                                 "Initial loopback clients timed out under one-core/full-cache-off conditions; the inherited harness checked joins only and initially returned a false positive, which was rejected after reading connection logs.",
                                 "Daily console schedule is unsupported; monthly schedule did not reach its trigger in the finite interval, so no client progress claim was accepted.",
                                 "Read-only progress probe under one-core allocation reached the catch-up wait limit; not accepted as a pass."],
                metadata_note="The first pruned build metadata listed an unused leftover baseline TU copy; its project compiled repository passenger_service_database.cpp. Source SHA above matches the detailed build and routing code used throughout.")
(output / "verification-metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
print(result)
