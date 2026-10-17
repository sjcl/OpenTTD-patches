"""Freeze an 8192-tick Hyperpath seed, verify its reload, then capture a new WPR pair."""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
WORK = Path(__file__).resolve().parent
BUILD = ROOT / 'build-wpr-8462e6cd'
ORIGINAL = Path('C:/Users/bebe/.codex/worktrees/5a69/OpenTTD-patches/build-hyperpath-cpu/frozen')
PACKAGE = WORK / 'frozen-8192'
RUNNER = ROOT / 'utils/passenger_precomputed_real_map.py'


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def dump(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def checked_run(command, log, env=None):
    print('Running: ' + log.name, flush=True)
    with log.open('wb') as stream:
        subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, env=env, check=True)


def verify_run(name, ticks):
    meta = read(WORK / name / 'process.json')
    assert meta['exit_code'] == 0
    assert not meta['contamination'] and not meta['phase_contamination']
    state = read(WORK / name / 'save-integrity.json')
    assert state['payments_valid'] is True
    if ticks:
        clock = meta['actual-ticks.json']
        assert clock['end_tick'] - clock['start_tick'] == len(clock['samples']) == ticks
        assert meta['simulation_timing_valid']
        assert clock['end_tick'] == state['state_ticks']
    return meta, state


deadline = time.monotonic() + 3000
while not (WORK / 'warmup/process.json').is_file():
    if time.monotonic() > deadline:
        raise TimeoutError('8192-tick warmup did not finish')
    time.sleep(2)
warm, seed = verify_run('warmup', 8192)
assert warm['actual-ticks.json']['start_tick'] == 3181119581
assert seed['state_ticks'] == 3181127773
assert warm['hyperpath'] is True and warm['hyperpath_filter_mode'] == 'index'
assert sha(WORK / 'warmup/result.sav') == warm['result_sha256']
checked_run([sys.executable, '-B', str(RUNNER), 'freeze', str(WORK / 'warmup/result.sav'),
             str(ORIGINAL), str(BUILD), str(PACKAGE)], WORK / 'freeze.log')
manifest = read(PACKAGE / 'package.json')
assert manifest['input_sha256'] == warm['result_sha256']
assert sha(PACKAGE / 'input.sav') == warm['result_sha256']
dump(WORK / 'frozen-seed.json', dict(product_commit='8462e6cd8280fe399fbb3b3e4a9cdb4413c935b4',
    original_input_sha256=warm['input_sha256'], warmup_ticks=8192,
    start_tick=3181119581, frozen_tick=seed['state_ticks'],
    frozen_input_sha256=manifest['input_sha256'], frozen_package=str(PACKAGE),
    canonical=seed))
print('Frozen seed: ' + manifest['input_sha256'], flush=True)

env = dict(os.environ)
for key in ['OPENTTD_PASSENGER_TRACE', 'OPENTTD_PASSENGER_CHECKPOINT_TICK',
            'OPENTTD_PASSENGER_FOCUS_TRACE', 'OPENTTD_PASSENGER_DEPARTURE_DIAGNOSTIC',
            'OPENTTD_RETIREMENT_PROFILE', 'OPENTTD_RETIREMENT_REFERENCE']:
    env.pop(key, None)

common = [sys.executable, '-B', str(RUNNER), 'run', str(BUILD / 'Release/openttd.exe'), str(PACKAGE)]
checked_run(common + [str(WORK / 'reload'), '--ticks', '0', '--timeout', '2400',
            '--measurement-wait', '120', '--resume', '--main-thread-affinity-mask', '1',
            '--lang', str(BUILD / 'lang')], WORK / 'reload-runner.log', env)
reload, loaded = verify_run('reload', 0)
differences = {key: dict(saved=seed.get(key), reloaded=loaded.get(key))
               for key in seed.keys() | loaded.keys() if seed.get(key) != loaded.get(key)}
dump(WORK / 'reload-validation.json', dict(equal=not differences, fields=len(seed), differences=differences,
    input_sha256=reload['input_sha256'], frozen_tick=seed['state_ticks']))
assert not differences, differences
print('Reload canonical: all ' + str(len(seed)) + ' fields match', flush=True)

for name in ['legacy', 'hyperpath']:
    command = common + [str(WORK / name), '--ticks', '2048', '--timeout', '2400',
        '--measurement-wait', '120', '--resume', '--precomputed',
        '--no-hyperpath' if name == 'legacy' else '--hyperpath',
        '--warm-precomputed', '--wait-background-ready', '--hyperpath-filter-mode', 'index',
        '--main-thread-affinity-mask', '1', '--lang', str(BUILD / 'lang'), '--wpr-profile']
    checked_run(command, WORK / (name + '-runner.log'), env)
    meta, state = verify_run(name, 2048)
    assert meta['actual-ticks.json']['start_tick'] == seed['state_ticks']
    assert meta['input_sha256'] == manifest['input_sha256']
    assert meta['cold_precomputed'] is False and meta['resume_settings'] is True
    assert meta['warm_complete'] is True and meta['background_ready']['completed'] is True
    assert meta['hyperpath'] == (name == 'hyperpath')
    console = (WORK / name / 'on-1-console.txt').read_text(encoding='utf-8', errors='replace')
    lookup = re.findall(r'Passenger hyperpath lookup: rebuilds=(\d+); indexed_groups=(\d+); validated_groups=(\d+); violations=(\d+)', console)
    assert lookup and int(lookup[-1][3]) == 0
    print('Completed ' + name + ': ticks=2048; lookup=' + str(lookup[-1]), flush=True)
print('New steady-input WPR pair completed', flush=True)
