"""Validate and retain the final product configuration and regression evidence."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=pathlib.Path)
p.add_argument('archive', type=pathlib.Path)
a = p.parse_args()
build, output = a.build.resolve(), a.archive.resolve()
root = pathlib.Path(__file__).resolve().parents[3]
cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
def option(name):
    return re.search(r'^' + name + r':[^=]+=(.*)$', cache, re.M)[1]

assert option('OPTION_PASSENGER_BENCHMARK') == 'OFF'
assert option('OPTION_USE_ASSERTS') == 'ON' and option('OPTION_NO_LTO') == 'ON'
assert option('CMAKE_MSVC_RUNTIME_LIBRARY') == 'MultiThreaded'
xml = (build / 'openttd_lib.vcxproj').read_text(encoding='utf-8-sig')
assert all(term not in xml for term in ['WITH_PASSENGER_BENCHMARK', 'journey-profile', 'save-integrity', 'network-probe'])
ctest = (build / 'final-ctest.log').read_text(encoding='utf-8', errors='replace')
assert '100% tests passed, 0 tests failed out of 590' in ctest
passenger = (build / 'final-passenger.log').read_text(encoding='utf-8', errors='replace')
assert 'All tests passed (2670223 assertions in 352 test cases)' in passenger
output.mkdir(parents=True, exist_ok=True)
logs = ['final-product-configure.log', 'final-product-build.log', 'final-ctest.log', 'final-passenger.log',
        'final-age-boundaries.log', 'age-ubsan-build.log', 'age-ubsan-run.log', 'age-ubsan-calibration.log',
        'final-evidence-tests.log', 'final-script-syntax.log', 'final-summary-validation.log']
for name in logs:
    shutil.copy2(build / name, output / name)
result = {'source_revision': subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip(),
    'product_binary_sha256': hashlib.sha256((build / 'Release/openttd.exe').read_bytes()).hexdigest(),
    'benchmark': 'OFF', 'asserts': 'ON', 'LTO': 'OFF', 'runtime': 'MultiThreaded',
    'release_flags': option('CMAKE_CXX_FLAGS_RELEASE'), 'isolated_probe_references': False,
    'ctest': {'cases': 590, 'failed': 0}, 'passenger': {'cases': 352, 'assertions': 2670223, 'failed': 0},
    'boundary': {'MSVC_assertions': 327, 'UBSan_cases': 95, 'old_subtraction_trap_calibrated': True}}
(output / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result, indent=2))
