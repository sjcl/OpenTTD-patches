"""Run Hyperpath validate exactly once; exclude correctness-only timings."""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORK = Path(__file__).resolve().parent
BASE = ROOT / 'docs/measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19'
HIST = ROOT / 'docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
REVISION = '6aeaff68fc016a83eeca44d4484c9fc8024cabef'

def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))

def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def diagnostic(console, name):
    lines = re.findall(r'^\s*Passenger hyperpath ' + name + r': (.+)$', console, re.MULTILINE)
    assert len(lines) == 1, (name, lines)
    values = {}
    for field in lines[0].strip().split('; '):
        key, value = field.split('=', 1)
        values[key] = int(value)
    return lines[0].strip(), values

def compare(reference, actual):
    assert len(reference) == len(actual) == 24
    differences = {key: [reference.get(key), actual.get(key)] for key in sorted(reference.keys() | actual.keys())
                   if reference.get(key) != actual.get(key)}
    return dict(equal=not differences, fields=24, differences=differences)

assert (WORK / 'build-ready.txt').is_file()
identity = load(WORK / 'build-identity.json')
assert identity['product_commit'] == REVISION
for name, item in identity['files'].items():
    assert sha(WORK / name) == item['sha256'], name
inputs = load(WORK / 'input-identity.json')
assert sha(WORK / 'input/input.sav') == inputs['input_sha256']
assert not (WORK / 'validate').exists(), 'Never overwrite or re-run an existing validation'
command = [sys.executable, '-B', str(ROOT / 'utils/passenger_precomputed_real_map.py'), 'run',
           str(WORK / 'Release/openttd.exe'), str(WORK / 'input'), str(WORK / 'validate'),
           '--ticks', '2048', '--timeout', '2400', '--measurement-wait', '2400', '--resume',
           '--wait-saved-replay', '--precomputed', '--hyperpath', '--warm-precomputed',
           '--wait-background-ready', '--hyperpath-filter-mode', 'validate',
           '--main-thread-affinity-mask', '1', '--lang', str(WORK / 'lang'), '--validation-only']
env = {key: value for key, value in os.environ.items() if not key.startswith('OPENTTD_')}
dump(WORK / 'validate-command.json', dict(command=command, correctness_only=True, wpr=False,
     scope='Exactly one real-map run; concurrent PGO training is recorded, not stopped; no performance claims.'))
with (WORK / 'validate-runner.log').open('wb') as output:
    subprocess.run(command, cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT, check=True)
meta = load(WORK / 'validate/process.json')
ticks = meta['actual-ticks.json']
assert meta['exit_code'] == 0 and not meta['load_failed']
assert meta['validation_only'] and not meta['simulation_timing_valid'] and not meta['wpr_profile']
assert meta['saved_replay']['ready'] and meta['warm_complete'] and meta['background_ready']['completed']
assert meta['background_ready']['tick_before'] == meta['background_ready']['tick_after'] == inputs['start_tick']
assert ticks['start_tick'] == inputs['start_tick'] and ticks['end_tick'] == inputs['target_end_tick']
assert len(ticks['samples']) == ticks['end_tick'] - ticks['start_tick'] == 2048
assert meta['hyperpath'] is True and meta['hyperpath_filter_mode'] == 'validate'
assert meta['input_sha256'] == inputs['input_sha256']
assert meta['binary_sha256'] == identity['files']['Release/openttd.exe']['sha256']
console = (WORK / 'validate/on-1-console.txt').read_text(encoding='utf-8-sig')
lookup_line, lookup = diagnostic(console, 'lookup')
memo_line, memo = diagnostic(console, 'memo')
assert set(memo) == {'hits', 'validated', 'conservative', 'violations', 'lookup_duplicates'}, memo
assert lookup['validated_groups'] > 0 and memo['validated'] > 0
actual = meta['save-integrity.json']
assert actual['payments_valid']
canonical = {
    '0b8a21c0_validate': compare(load(BASE / 'validate/save-integrity.json'), actual),
    '0b8a21c0_hyperpath': compare(load(BASE / 'hyperpath/save-integrity.json'), actual),
    '97cdc0ad_hyperpath': compare(load(HIST / 'hyperpath/save-integrity.json'), actual),
}
passed = lookup['violations'] == memo['violations'] == memo['lookup_duplicates'] == 0
result = dict(product_commit=REVISION, command=command, exact_ticks=2048, start_tick=ticks['start_tick'],
              end_tick=ticks['end_tick'], input_sha256=meta['input_sha256'], correctness_only=True,
              simulation_timing_valid=False, excluded_from_performance_comparison=True, wpr=False,
              saved_replay=meta['saved_replay'], warm_complete=meta['warm_complete'],
              background_ready=meta['background_ready'], lookup=lookup, memo=memo,
              lookup_line=lookup_line, memo_line=memo_line, validation_passed=passed,
              canonical=canonical, script_error_pause_resumes=meta['script_error_pause_resumes'],
              note='Conservative recomputations are not violations at this revision; timings intentionally invalid for performance.')
dump(WORK / 'validate-result.json', result)
(WORK / 'validate-complete.txt').write_text('One requested validation completed; inspect validation_passed\n')
print(json.dumps(result, indent=2, ensure_ascii=False), flush=True)
