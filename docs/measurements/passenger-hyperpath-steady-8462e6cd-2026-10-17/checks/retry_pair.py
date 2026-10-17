"""Retry both WPR modes after a competing build invalidated the initial Legacy capture."""
from pathlib import Path
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
WORK = Path(__file__).resolve().parent
BUILD = ROOT / 'build-wpr-8462e6cd'
PACKAGE = WORK / 'frozen-8192'
assert json.loads((WORK / 'reload-validation.json').read_text())['equal'] is True
env = dict(os.environ)
for key in ['OPENTTD_PASSENGER_TRACE', 'OPENTTD_PASSENGER_CHECKPOINT_TICK',
            'OPENTTD_PASSENGER_FOCUS_TRACE', 'OPENTTD_PASSENGER_DEPARTURE_DIAGNOSTIC',
            'OPENTTD_RETIREMENT_PROFILE', 'OPENTTD_RETIREMENT_REFERENCE']:
    env.pop(key, None)
for mode in ['legacy', 'hyperpath']:
    command = [sys.executable, '-B', str(ROOT / 'utils/passenger_precomputed_real_map.py'), 'run',
        str(BUILD / 'Release/openttd.exe'), str(PACKAGE), str(WORK / mode), '--ticks', '2048',
        '--timeout', '2400', '--measurement-wait', '2400', '--resume', '--precomputed',
        '--no-hyperpath' if mode == 'legacy' else '--hyperpath', '--warm-precomputed',
        '--wait-background-ready', '--hyperpath-filter-mode', 'index',
        '--main-thread-affinity-mask', '1', '--lang', str(BUILD / 'lang'), '--wpr-profile']
    print('Beginning clean ' + mode, flush=True)
    with (WORK / (mode + '-runner.log')).open('wb') as stream:
        subprocess.run(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
    meta = json.loads((WORK / mode / 'process.json').read_text())
    clock = meta['actual-ticks.json']
    assert meta['exit_code'] == 0 and meta['simulation_timing_valid']
    assert not meta['contamination'] and not meta['phase_contamination']
    assert clock['end_tick'] - clock['start_tick'] == len(clock['samples']) == 2048
    assert clock['start_tick'] == 3181127773 and clock['end_tick'] == 3181129821
    assert meta['input_sha256'] == '275544f1fd7bb16920ebd325c3a916ee06f164fcbe53abf0c1bb6409e9802f9f'
    assert meta['save-integrity.json']['payments_valid'] is True
    print('Completed clean ' + mode, flush=True)
print('Clean pair completed', flush=True)
