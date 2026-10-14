"""Build isolated admission experiments; production headers/settings remain fixed.

Run after configuring/building the product. Copies only active.cpp and the existing
minimal console instrumentation; never adds timers or wall-clock admission rules.
"""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
VARIANTS = {"control": None, "q32-t49152": (32, 49152, 16384),
            "q64-t49152": (64, 49152, 16384), "q32-t32768": (32, 32768, 8192)}


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected one anchor: {old}")
    return text.replace(old, new)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("build", type=pathlib.Path)
    p.add_argument("--variants", nargs="+", choices=VARIANTS, default=list(VARIANTS))
    p.add_argument("--control", default="8a96044d40")
    p.add_argument("--parallel", type=int, default=8)
    a = p.parse_args()
    build = a.build.resolve()
    subprocess.run([sys.executable, "-B", str(HERE.parent / "passenger-routing-profile-2026-10-13/Instrument-Profile.py"),
                    str(ROOT), str(build), "--mode", "minimal"], check=True)
    project = build / "openttd_lib.vcxproj"
    xml = project.read_text(encoding="utf-8-sig")
    original = ROOT / "src/passenger_journey_active.cpp"
    target = build / "journey-profile/passenger_journey_active.cpp"
    project.write_text(replace_once(xml, str(original), str(target)), encoding="utf-8-sig")
    base_metadata = json.loads((build / "journey-profile/instrumentation.json").read_text(encoding="utf-8"))
    current = original.read_text(encoding="utf-8")
    for name in a.variants:
        parameters = VARIANTS[name]
        text = current if parameters else subprocess.check_output(["git", "-C", str(ROOT), "show",
                    f"{a.control}:src/passenger_journey_active.cpp"], text=True, encoding="utf-8")
        if parameters:
            cap, timed, protection = parameters
            for old, new in (("PERIODIC_QUERY_LIMIT = PassengerQueryAdmission::QUERIES_PER_TICK", f"PERIODIC_QUERY_LIMIT = {cap}"),
                             ("PERIODIC_TIMED_WORK = 49152", f"PERIODIC_TIMED_WORK = {timed}"),
                             ("PERIODIC_SCHEDULE_PROTECTION = PassengerQueryAdmission::SCHEDULE_PROTECTION", f"PERIODIC_SCHEDULE_PROTECTION = {protection}")):
                text = replace_once(text, old, new)
        text = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (ROOT / "src" / m[1]).as_posix() + '"'
                      if (ROOT / "src" / m[1]).is_file() else m[0], text)
        target.write_text(text, encoding="utf-8")
        metadata = dict(base_metadata, admission_variant=name, parameters=parameters, control_commit=a.control,
                        active_copy_sha256=hashlib.sha256(target.read_bytes()).hexdigest())
        with (build / f"build-{name}.log").open("w", encoding="utf-8") as output:
            subprocess.run(["cmake", "--build", str(build), "--config", "Release", "--target", "openttd", "--parallel", str(a.parallel)],
                           stdout=output, stderr=subprocess.STDOUT, check=True)
        binary = build / f"openttd-{name}.exe"
        shutil.copy2(build / "Release/openttd.exe", binary)
        metadata["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        binary.with_suffix(".instrumentation.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        print(name, metadata["binary_sha256"], flush=True)


if __name__ == "__main__":
    main()
