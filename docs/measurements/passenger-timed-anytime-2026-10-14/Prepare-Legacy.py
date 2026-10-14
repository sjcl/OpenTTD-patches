"""Use the old completion rule in a copied TU for a matched performance control.

Shares the final lightweight diagnostics with Anytime. This is a benchmark-only
binary, never a player preference or a network setting. Search order/budgets are
checked by the every-Work-boundary regression and sampled quality probe.
"""
import argparse
import hashlib
import json
import pathlib
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("source", type=pathlib.Path)
p.add_argument("build", type=pathlib.Path)
p.add_argument("--original", action="store_true", help="Retain the original generation handling for observational diagnosis")
a = p.parse_args()
source, build = a.source.resolve(), a.build.resolve()
original = source / "src/passenger_service_database.cpp"
text = original.read_text(encoding="utf-8")
anchor = "return this->PlanInternal(cargo, origin, destination, ready, limits, evaluation, excluded, true);"
assert text.count(anchor) == 1
completion = "PlanWithOriginalCompletion" if a.original else "PlanWithLegacyCompletion"
text = text.replace(anchor, "return this->" + completion + "(cargo, origin, destination, ready, limits, evaluation, excluded);")
text = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (source / "src" / m[1]).as_posix() + '"', text)
# CMake's OBJECT library also puts explicit basename.obj inputs in consumers.
# Keep the original basename so the exe links the replacement translation unit.
target = build / ("journey-profile/original" if a.original else "journey-profile/legacy") / "passenger_service_database.cpp"
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text(text, encoding="utf-8")
project = build / "openttd_lib.vcxproj"
xml = project.read_text(encoding="utf-8-sig")
assert xml.count(str(original)) == 1
project.write_text(xml.replace(str(original), str(target)), encoding="utf-8-sig")
path = build / "journey-profile/instrumentation.json"
metadata = json.loads(path.read_text(encoding="utf-8"))
metadata["legacy_completion"] = {"original_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
	"original_generation_handling": a.original,
    "copy_sha256": hashlib.sha256(target.read_bytes()).hexdigest(), "diagnostics": "shared_final_lazy_bounds"}
path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
print(target)
