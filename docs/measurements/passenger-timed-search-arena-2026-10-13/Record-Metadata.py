"""Record the measured sources, binaries, compiler, assets and known excluded trials."""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("build", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
a = p.parse_args()
root = pathlib.Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("runner", root / "docs/measurements/passenger-routing-profile-2026-10-13/Run-Profile.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
files = ["src/passenger_service_database.cpp", "src/passenger_service_database.h", "src/passenger_timed_search.h", "src/console_cmds.cpp", "src/tests/passenger_service_database.cpp"]
sources = {name: runner.sha256(root / name) for name in files}
variants = {}
for mode in ("legacy", "arena", "disabled", "legacy-detail", "arena-detail", "network"):
    variants[mode] = dict(binary_sha256=runner.sha256(a.build / f"openttd-{mode}.exe"),
                          instrumentation=json.loads((a.build / f"openttd-{mode}.instrumentation.json").read_text(encoding="utf-8")))
for mode in ("arena", "disabled", "network"):
    for name, expected in variants[mode]["instrumentation"]["source_sha256"].items():
        assert sources["src/" + name] == expected, f"Measured product source changed: {name}"
result = dict(schema_version=1, base_commit="604ce993892479656a2c5ba2fbd311cb19c5d896",
              branch=subprocess.check_output(["git", "-C", str(root), "branch", "--show-current"], encoding="utf-8").strip(),
              compiler=runner.compiler_info(a.build), source_sha256=sources, variants=variants,
              final_product_binary_sha256=runner.sha256(a.build / "Release/openttd.exe"),
              final_test_binary_sha256=runner.sha256(a.build / "Release/openttd_test.exe"),
              network_probe_sha256=runner.sha256(a.build / "network-probe/null_v.cpp"),
              excluded_trials=["Initial full passenger run: benchmark leaked disabled cache preferences; fixed with RAII restoration.",
                               "Initial incomplete CTest invocation and unavailable root regression assets; final complete 570-test run passed.",
                               "Initial 1000-stop line fixture was rejected by the existing publication work ceiling; replaced by 32 certified 250-stop lines."],
              synthetic_allocator_in_game=False, synthetic_fixed_scratch_bytes=8312,
              mp_client_progress_resolution_ticks=128, mp_peer_missed_sync_frame_messages=15)
a.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print("Source hashes verified; metadata written:", a.output)
