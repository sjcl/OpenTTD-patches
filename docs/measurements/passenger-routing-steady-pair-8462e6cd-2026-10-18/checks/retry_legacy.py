"""Retry only Legacy WPR after the independent seed and reload have passed."""
from pathlib import Path
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
WORK = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'utils'))
import passenger_precomputed_real_map as real

assert json.loads((WORK / 'reload-validation.json').read_text())['equal'] is True
print('Waiting for an idle host for 60 seconds', flush=True)
real.PROFILE.wait_for_idle(2400, 60)
env = dict(os.environ)
for key in ['OPENTTD_PASSENGER_TRACE', 'OPENTTD_PASSENGER_CHECKPOINT_TICK',
            'OPENTTD_PASSENGER_FOCUS_TRACE', 'OPENTTD_PASSENGER_DEPARTURE_DIAGNOSTIC',
            'OPENTTD_RETIREMENT_PROFILE', 'OPENTTD_RETIREMENT_REFERENCE']:
    env.pop(key, None)
command = [sys.executable, '-B', str(ROOT / 'utils/passenger_precomputed_real_map.py'), 'run',
    str(ROOT / 'build-wpr-8462e6cd/Release/openttd.exe'), str(WORK / 'frozen-8192'), str(WORK / 'legacy'),
    '--ticks', '2048', '--timeout', '2400', '--measurement-wait', '2400', '--resume', '--precomputed',
    '--no-hyperpath', '--warm-precomputed', '--wait-background-ready', '--hyperpath-filter-mode', 'index',
    '--main-thread-affinity-mask', '1', '--lang', str(ROOT / 'build-wpr-8462e6cd/lang'), '--wpr-profile']
with (WORK / 'legacy-runner.log').open('wb') as stream:
    subprocess.run(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
meta = json.loads((WORK / 'legacy/process.json').read_text())
clock = meta['actual-ticks.json']
assert meta['exit_code'] == 0 and meta['simulation_timing_valid']
assert not meta['contamination'] and not meta['phase_contamination']
assert len(clock['samples']) == clock['end_tick'] - clock['start_tick'] == 2048
assert clock['start_tick'] == 3181127773 and clock['end_tick'] == 3181129821
assert meta['input_sha256'] == 'b276c5de400651f646c129ab394bc97a686f38aa345b3a4a799e07c8a06289ca'
assert meta['save-integrity.json']['payments_valid'] is True
rejected = json.loads((WORK / 'legacy-contaminated/save-integrity.json').read_text())
assert rejected == meta['save-integrity.json']
(WORK / 'measurement-complete.json').write_text(json.dumps(dict(ticks=2048, mode='legacy',
    retry=True, canonical_matches_rejected=True, input_sha256=meta['input_sha256']))+'\n', encoding='utf-8')
print('Clean Legacy retry completed; canonical 24/24 matches rejected attempt', flush=True)
