"""Verify the frozen input, canonical reload, exact tick windows and lossless exports."""
from pathlib import Path
import csv
import gzip
import hashlib
import json
import os
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
WORK = Path(__file__).resolve().parent
EVIDENCE = ROOT / 'docs/measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17'
sys.path.insert(0, str(ROOT / 'utils'))
import passenger_precomputed_real_map as real


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def sha(path):
    return real.PROFILE.sha256(path)


package = WORK / 'frozen-8192'
manifest = load(package / 'package.json')
assert sha(package / 'input.sav') == manifest['input_sha256']
assert sha(package / 'openttd.cfg') == manifest['config_sha256']
assert real.PROFILE.inventory(package) == manifest['assets']
seed = load(WORK / 'warmup/save-integrity.json')
assert seed == load(WORK / 'reload/save-integrity.json')
assert seed['state_ticks'] == 3181127773 and seed['payments_valid'] is True and len(seed) == 24
subprocess.run(['git', 'diff', '--quiet', '8462e6cd', '--', 'src'], cwd=ROOT, check=True)
checks = dict(product_sources_unchanged=True, frozen_package_all_assets_match=True,
    reload_equal_fields=len(seed), frozen_input_sha256=manifest['input_sha256'], runs={}, exports={})

for name, ticks, start in [('warmup', 8192, 3181119581), ('reload', 0, 3181127773),
                           ('legacy', 2048, 3181127773), ('hyperpath', 2048, 3181127773)]:
    directory = WORK / name
    meta = load(directory / 'process.json')
    state = load(directory / 'save-integrity.json')
    assert meta['exit_code'] == 0 and state['payments_valid'] is True
    assert not meta['contamination'] and not meta['phase_contamination']
    assert meta['save-integrity.json'] == state
    assert sha(directory / 'result.sav') == meta['result_sha256']
    assert sha(directory / 'input.sav') == meta['input_sha256']
    assert meta['binary_sha256'] == sha(WORK / 'Release/openttd.exe')
    if ticks:
        clock = load(directory / 'actual-ticks.json')
        assert clock == meta['actual-ticks.json']
        assert len(clock['samples']) == clock['end_tick'] - clock['start_tick'] == ticks
        assert clock['start_tick'] == start and clock['end_tick'] == state['state_ticks'] == start + ticks
        assert [sample[0] for sample in clock['samples']] == list(range(start + 1, start + ticks + 1))
        assert meta['simulation_timing_valid'] is True
    else:
        assert state['state_ticks'] == start
    if name != 'warmup':
        assert meta['input_sha256'] == manifest['input_sha256']
        assert meta['resume_settings'] is True and meta['cold_precomputed'] is False
    checks['runs'][name] = dict(exit_code=0, ticks=ticks, start_tick=start,
        end_tick=state['state_ticks'], payments_valid=True, contamination=0, result_sha256=meta['result_sha256'])

summary = load(EVIDENCE / 'summary.json')
for mode in ['legacy', 'hyperpath']:
    meta = load(WORK / mode / 'process.json')
    assert meta['hyperpath'] == (mode == 'hyperpath')
    assert meta['warm_complete'] is True and meta['background_ready']['completed'] is True
    info = summary[mode]
    assert info['lost_events'] == info['lost_buffers'] == 0
    assert not info['unresolved_product_functions']
    for scope, totals in info['conservation_checks'].items():
        assert totals['stack_total_ns'] == totals['exclusive_total_ns']
        assert totals['stack_samples'] == totals['exclusive_samples']
    export = load(WORK / mode / 'export-command.json')
    assert export['exit_code'] == 0 and export['pdb_sha256'] == sha(WORK / 'Release/openttd.pdb')
    for name, item in export['exports'].items():
        compressed = EVIDENCE / mode / (name + '.gz')
        with gzip.open(compressed, 'rb') as stream:
            actual = hashlib.file_digest(stream, 'sha256').hexdigest()
        assert actual == item['sha256'] == sha(WORK / mode / 'wpa' / name)
    checks['exports'][mode] = dict(csv_count=len(export['exports']), raw_gzip_sha256_match=True,
        lost_events=0, lost_buffers=0, unresolved_product_functions=0, conservation_checks_passed=True)
assert (WORK / 'wpr-recorder-complete.txt').is_file()
original = Path('C:/Users/bebe/.codex/worktrees/5a69/OpenTTD-patches/build-hyperpath-cpu/frozen/input.sav')
assert sha(original) == '306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629'
checks['original_input_unchanged'] = True
# The source save is immutable measurement input; all replays write result.sav elsewhere.
os.chmod(package / 'input.sav', stat.S_IREAD)
checks['frozen_save_read_only'] = not bool((package / 'input.sav').stat().st_mode & stat.S_IWRITE)
assert checks['frozen_save_read_only']
(EVIDENCE / 'checks/validation.json').write_text(json.dumps(checks, indent=2) + '\n', encoding='utf-8')
print(json.dumps(checks, indent=2))
