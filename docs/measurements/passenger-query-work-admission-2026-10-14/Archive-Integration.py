"""Validate and archive merged-base checks without committing saves or binaries."""
import argparse
import gzip
import hashlib
import json
import pathlib
import re
import shutil
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=pathlib.Path)
p.add_argument('archive', type=pathlib.Path)
p.add_argument('--skip-performance', action='store_true', help='Explicit user authorization to omit the six-run CPU comparison')
a = p.parse_args()
build, output = a.build.resolve(), a.archive.resolve()
checks = output / 'checks'
checks.mkdir(parents=True, exist_ok=True)
cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')

def option(name):
    return re.search(r'^' + name + r':[^=]+=(.*)$', cache, re.M)[1]

assert option('OPTION_PASSENGER_BENCHMARK') == 'OFF'
assert option('OPTION_USE_ASSERTS') == 'ON' and option('OPTION_NO_LTO') == 'ON'
assert option('CMAKE_MSVC_RUNTIME_LIBRARY') == 'MultiThreaded'
xml = (build / 'openttd_lib.vcxproj').read_text(encoding='utf-8-sig')
assert all(term not in xml for term in ['WITH_PASSENGER_BENCHMARK', 'journey-profile', 'save-integrity', 'network-probe', 'network-timeline'])
prefix = 'integration-2026-10-15-'
ctest = (build / (prefix + 'ctest.log')).read_text(encoding='utf-8', errors='replace')
passenger = (build / (prefix + 'passenger.log')).read_text(encoding='utf-8', errors='replace')
ctest_count = int(re.search(r'100% tests passed, 0 tests failed out of (\d+)', ctest)[1])
assert ctest_count >= 600
match = re.search(r'All tests passed \((\d+) assertions in (\d+) test cases\)', passenger)
assert int(match[1]) >= 2671173 and int(match[2]) >= 362
for suffix in ['configure', 'build', 'merge-regression', 'test-build', 'passenger', 'ctest',
               'profile-configure', 'profile-build', 'comparison', 'saveload', 'v4', 'cache-check',
               'mp-configure', 'mp-prepare', 'mp-build', 'mp-run',
               'product-configure', 'product-build', 'script-syntax']:
    name = prefix + suffix + '.log'
    shutil.copy2(build / name, checks / name)
save_root = build / 'integration-2026-10-15-saveload'
save_result = json.loads((save_root / 'result.json').read_text(encoding='utf-8'))
assert all(save_result['checks'].values())
save_out = output / 'saveload'
save_out.mkdir(exist_ok=True)
for name in ['result.json', 'configure.log', 'prepare-integrity.log', 'prepare-driver.log', 'build.log']:
    shutil.copy2(save_root / name, save_out / name)
for name, stage in save_result['stages'].items():
    target = save_out / name
    target.mkdir(exist_ok=True)
    shutil.copy2(save_root / (name + '.log'), target / 'runner.log')
    for filename in ['stage.json', 'save-integrity.json', 'continuation-driver.json', 'on-1-console.txt', 'stdout.txt']:
        if (save_root / name / filename).exists():
            shutil.copy2(save_root / name / filename, target / filename)
    raw = (save_root / name / 'stderr.txt').read_bytes()
    (target / 'stderr.txt.gz').write_bytes(gzip.compress(raw, mtime=0))
    (target / 'stderr-integrity.json').write_text(json.dumps({'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}, indent=2) + '\n', encoding='utf-8')
network = json.loads((output / 'multiplayer/result.json').read_text(encoding='utf-8'))
network_run = json.loads((output / 'multiplayer/run-metadata.json').read_text(encoding='utf-8'))
assert network['cargo_state_equal'] and not network['detected_sync_failure'] and network['connected_clients_at_end'] == 2
assert network['client_cache_mode'] == 'warm' and not network_run['contamination']
cache_result = json.loads((save_root / 'cache-determinism.json').read_text(encoding='utf-8'))
assert all(cache_result['checks'].values())
shutil.copy2(save_root / 'cache-determinism.json', save_out / 'cache-determinism.json')
if not a.skip_performance:
    profile = json.loads((output / 'profile/summary.json').read_text(encoding='utf-8'))
    assert all(profile['cache_determinism'].values())
revision = subprocess.check_output(['git', '-C', ROOT, 'rev-parse', 'HEAD'], text=True).strip()
assert revision == save_result['source_revision'] == network['source_revision']
if not a.skip_performance:
    assert revision == profile['source_revision']
result = {'source_revision': revision,
          'control_commit': subprocess.check_output(['git', '-C', ROOT, 'rev-parse', 'a6e6664a18'], text=True).strip(),
          'product_binary_sha256': hashlib.sha256((build / 'Release/openttd.exe').read_bytes()).hexdigest(),
          'benchmark': 'OFF', 'asserts': 'ON', 'LTO': 'OFF', 'runtime': option('CMAKE_MSVC_RUNTIME_LIBRARY'),
          'release_flags': option('CMAKE_CXX_FLAGS_RELEASE'), 'isolated_probe_references': False,
          'ctest': {'cases': ctest_count, 'failed': 0},
          'passenger': {'cases': int(match[2]), 'assertions': int(match[1]), 'failed': 0},
          'performance_comparison': 'skipped by explicit user request' if a.skip_performance else 'complete',
          'cache_determinism': cache_result['checks'], 'saveload': save_result['checks'],
          'warm_multiplayer': {'connected_clients': 2, 'cargo_equal': True, 'post_join_seconds': network['post_late_join_seconds'], 'uncontaminated': True},
          'all_cache_off_network': 'separate receive-fairness task; prior failure retained'}
(output / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result, indent=2))
