"""Add sampled same-state legacy/high-Work comparisons to a copied source TU.

This separate binary is for quality only. Its latency is excluded from the
performance comparison. Neither probe updates the source DB, packets or RNG.
"""
import argparse
import hashlib
import json
import pathlib
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("source", type=pathlib.Path)
p.add_argument("build", type=pathlib.Path)
p.add_argument("--stride", type=int, default=1024)
a = p.parse_args()
if a.stride < 1:
    p.error("stride must be positive")
source, build = a.source.resolve(), a.build.resolve()
original = source / "src/passenger_service_database.cpp"
text = original.read_text(encoding="utf-8")
text = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (source / "src" / m[1]).as_posix() + '"', text)
text = '#include <fstream>\n' + text
anchor = "\t\treturn result;\n\t};\n\tif (cargo >= NUM_CARGO"
assert text.count(anchor) == 1
probe = r'''
        static bool comparing = false;
        if (!comparing && this->diagnostic.searches % STRIDE == 1) {
            struct Guard { bool &flag; Guard(bool &flag) : flag(flag) { flag = true; } ~Guard() { flag = false; } } guard{comparing};
            PassengerServiceDatabase isolated;
            if (isolated.Load(this->Save())) {
                isolated.FinishLoad(true);
                auto old = isolated.PlanWithLegacyCompletion(cargo, origin, destination, ready, limits, evaluation, excluded);
                auto high_limits = limits; high_limits.work = 1048576; high_limits.labels = 65536; high_limits.memory_bytes = 128 * 1024 * 1024;
                auto reference = isolated.Plan(cargo, origin, destination, ready, high_limits, evaluation, excluded);
                std::ofstream out("anytime-quality.jsonl", std::ios::app);
                out << "{\"ordinal\":" << this->diagnostic.searches << ",\"cargo\":" << unsigned(cargo)
                    << ",\"origin\":" << origin.base() << ",\"destination\":" << destination.base()
                    << ",\"ready\":" << ready.base() << ",\"excluded\":" << excluded.base()
                    << ",\"work\":" << result.work << ",\"state\":" << unsigned(result.state)
                    << ",\"reason\":" << unsigned(result.reason) << ",\"found\":" << bool(result.ride)
                    << ",\"cost\":" << result.cost << ",\"arrival\":" << result.arrival.base()
                    << ",\"old_found\":" << bool(old.ride) << ",\"old_deferred\":" << old.deferred
                    << ",\"old_cost\":" << old.cost << ",\"old_arrival\":" << old.arrival.base()
                    << ",\"old_work\":" << old.work << ",\"reference_found\":" << bool(reference.ride)
                    << ",\"reference_certified\":" << (reference.state == PassengerPeriodicPlan::State::Certified)
                    << ",\"reference_cost\":" << reference.cost << ",\"reference_arrival\":" << reference.arrival.base()
                    << ",\"reference_work\":" << reference.work << "}\n";
            }
        }
'''.replace("STRIDE", str(a.stride))
text = text.replace(anchor, probe + anchor)
target = build / "journey-profile/anytime-quality.cpp"
target.write_text(text, encoding="utf-8")
project = build / "openttd_lib.vcxproj"
xml = project.read_text(encoding="utf-8-sig")
assert xml.count(str(original)) >= 1
project.write_text(xml.replace(str(original), str(target)), encoding="utf-8-sig")
metadata_path = build / "journey-profile/instrumentation.json"
metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
metadata["quality_probe"] = {"stride": a.stride, "reference_work": 1048576,
    "original_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
    "copy_sha256": hashlib.sha256(target.read_bytes()).hexdigest(), "performance_eligible": False}
metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
print(target)
