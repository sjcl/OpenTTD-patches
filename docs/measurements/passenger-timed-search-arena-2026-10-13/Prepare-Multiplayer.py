"""Use the archived progress-checked loopback harness, disabling scratch on both peers."""
import argparse
import pathlib
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("build", type=pathlib.Path)
a = p.parse_args()
source = pathlib.Path(__file__).resolve().parents[3]
subprocess.run([sys.executable, str(source / "docs/measurements/passenger-timed-pruning-2026-10-13/Prepare-Multiplayer.py"), str(a.build),
                "--directory", "mp-arena", "--probe", "--unrestricted-clients"], check=True)
target = a.build / "Exercise-Pruning-Multiplayer.py"
text = target.read_text(encoding="utf-8")
assert text.count("passenger_journey_stats departures_off") == 2
text = text.replace("passenger_journey_stats departures_off", "passenger_journey_stats scratch_off\\npassenger_journey_stats departures_off")
anchor = '    (root / "result.json").write_text'
assert text.count(anchor) == 1
text = text.replace(anchor, '''    result["client_scratch_disabled"] = all("Timed search scratch: disabled" in text(directory / "console.txt") for directory in [first_dir, second_dir])
    result["client_scratch_retained_bytes"] = [[int(x) for x in re.findall(r"scratch_bytes=(\\d+);", text(directory / "progress-console.txt"))] for directory in [first_dir, second_dir]]
    if not result["client_scratch_disabled"] or not all(values and max(values) == 0 for values in result["client_scratch_retained_bytes"]):
        raise RuntimeError("Clients did not exercise disabled scratch")
''' + anchor)
target.write_text(text, encoding="utf-8")
print(target)
