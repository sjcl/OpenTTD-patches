"""Optional baseline clear trace; keep a separate uninstrumented performance binary.

Run against the jgrpp-ya baseline after configuring its Visual Studio build.
Only the generated project and a copied translation unit are modified.
"""
import argparse
import pathlib
import re

p = argparse.ArgumentParser()
p.add_argument("source", type=pathlib.Path)
p.add_argument("build", type=pathlib.Path)
a = p.parse_args()
base, build = a.source.resolve(), a.build.resolve()
s = (base / "src/passenger_service_database.cpp").read_text(encoding="utf-8")
s = s.replace('#include <chrono>', '#include <chrono>\n#include "debug.h"\nnamespace { const char *baseline_clear_reason = "explicit"; }')
s = s.replace('void PassengerServiceDatabase::ClearPolicies() {', 'void PassengerServiceDatabase::ClearPolicies() { Debug(misc, 0, "OD baseline clear: reason={}; timed={}; schedule={}", baseline_clear_reason, this->journey_policies.size(), this->schedule.GetDiagnostic().bytes / 1024); baseline_clear_reason = "explicit";')
s = s.replace('void PassengerServiceDatabase::Reset()\n{', 'void PassengerServiceDatabase::Reset()\n{\n\tbaseline_clear_reason = "reset";')
s = s.replace('++this->diagnostic.invalidations;\n\tthis->ClearPolicies();', '++this->diagnostic.invalidations;\n\tbaseline_clear_reason = id == OrderListID::Invalid() ? "global" : "structure";\n\tthis->ClearPolicies();')
s = s.replace('this->Remove(id); this->ClearPolicies();', 'this->Remove(id); baseline_clear_reason = "timing"; this->ClearPolicies();')
s = s.replace('this->services.emplace(id, std::move(service)); ++this->diagnostic.builds;\n\t\tthis->ClearPolicies();', 'this->services.emplace(id, std::move(service)); ++this->diagnostic.builds;\n\t\tbaseline_clear_reason = "publication"; this->ClearPolicies();')
s = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (base / 'src' / m.group(1)).as_posix() + '"', s)
target = build / "diagnostic/passenger_service_database.cpp"
target.parent.mkdir(exist_ok=True)
target.write_text(s, encoding="utf-8")
project = build / "openttd_lib.vcxproj"
text = project.read_text(encoding="utf-8-sig")
source = str(base / "src/passenger_service_database.cpp")
assert source in text, "Expected unmodified baseline project"
project.write_text(text.replace(source, str(target)), encoding="utf-8-sig")
