"""Check the independent Legacy seed, reused Hyperpath capture and exact tick intervals."""
from pathlib import Path
import gzip
import hashlib
import json
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
WORK = Path(__file__).resolve().parent
EVIDENCE = ROOT / 'docs/measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18'
PREVIOUS = ROOT / 'docs/measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17'
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
assert not ((package / 'input.sav').stat().st_mode & stat.S_IWRITE)
seed = load(WORK / 'warmup/save-integrity.json')
assert seed == load(WORK / 'reload/save-integrity.json') and len(seed) == 24
assert seed['state_ticks'] == 3181127773 and seed['payments_valid'] is True
hyperpath_seed = load(PREVIOUS / 'warmup/save-integrity.json')
assert hyperpath_seed == load(PREVIOUS / 'reload/save-integrity.json') and len(hyperpath_seed) == 24
assert hyperpath_seed['state_ticks'] == seed['state_ticks']
legacy_warm = load(WORK / 'warmup/process.json')
hyperpath_warm = load(PREVIOUS / 'warmup/process.json')
for key in ['input_sha256', 'binary_sha256', 'language_sha256', 'requested_ticks', 'precomputed',
            'cold_precomputed', 'warm_precomputed']:
    assert legacy_warm[key] == hyperpath_warm[key], key
subprocess.run(['git', 'diff', '--quiet', '8462e6cd', '--', 'src'], cwd=ROOT, check=True)
identity = load(PREVIOUS / 'build-identity.json')
assert sha(WORK / 'Release/openttd.exe') == identity['executable_sha256']
assert sha(WORK / 'Release/openttd.pdb') == identity['pdb_sha256']
checks = dict(product_sources_unchanged=True, same_binary_and_pdb=True,
    original_input_unchanged=True, frozen_legacy_save_read_only=True,
    frozen_legacy_assets_match=True, reload_equal_fields_per_mode=24, runs={}, exports={})
for name, ticks, start in [('warmup', 8192, 3181119581), ('reload', 0, 3181127773), ('legacy', 2048, 3181127773)]:
    directory = WORK / name
    meta = load(directory / 'process.json')
    state = load(directory / 'save-integrity.json')
    assert meta['exit_code'] == 0 and state['payments_valid'] is True
    if name == 'legacy':
        assert meta['simulation_timing_valid'] is True
        assert all(o['stage']=='after_simulation' for o in meta['phase_contamination'])
    else:
        assert not meta['contamination'] and not meta['phase_contamination']
    assert meta['save-integrity.json'] == state
    assert sha(directory / 'result.sav') == meta['result_sha256']
    assert sha(directory / 'input.sav') == meta['input_sha256']
    assert meta['binary_sha256'] == identity['executable_sha256']
    if ticks:
        clock = load(directory / 'actual-ticks.json')
        assert meta['simulation_timing_valid'] is True and clock == meta['actual-ticks.json']
        assert clock['start_tick'] == start and clock['end_tick'] == state['state_ticks'] == start + ticks
        assert len(clock['samples']) == ticks
        assert [row[0] for row in clock['samples']] == list(range(start + 1, start + ticks + 1))
        assert meta['hyperpath'] is False
    else:
        assert state['state_ticks'] == start
    if name != 'warmup':
        assert meta['input_sha256'] == manifest['input_sha256']
        assert meta['resume_settings'] is True and meta['cold_precomputed'] is False
    if name == 'legacy':
        assert meta['warm_complete'] is True and meta['background_ready']['completed'] is True
    checks['runs'][name] = dict(exit_code=0, ticks=ticks, start_tick=start, end_tick=state['state_ticks'],
        simulation_contamination=0, post_simulation_observations=len(meta['phase_contamination']),
        whole_process_timing_valid=meta['whole_process_timing_valid'],
        payments_valid=True, result_sha256=meta['result_sha256'])
summary = load(EVIDENCE / 'summary.json')
assert summary['hyperpath'] == load(PREVIOUS / 'summary.json')['hyperpath']
for mode in ['legacy', 'hyperpath']:
    item = summary[mode]
    assert item['start_tick'] == 3181127773 and item['end_tick'] == 3181129821
    assert item['lost_events'] == item['lost_buffers'] == 0 and not item['unresolved_product_functions']
    for totals in item['conservation_checks'].values():
        assert totals['stack_total_ns'] == totals['exclusive_total_ns']
        assert totals['stack_samples'] == totals['exclusive_samples']
    export = load(WORK / mode / 'export-command.json')
    assert export['exit_code'] == 0 and export['pdb_sha256'] == identity['pdb_sha256']
    for name, expected in export['exports'].items():
        with gzip.open(EVIDENCE / mode / (name + '.gz'), 'rb') as stream:
            actual = hashlib.file_digest(stream, 'sha256').hexdigest()
        assert actual == expected['sha256'] == sha(WORK / mode / 'wpa' / name)
    checks['exports'][mode] = dict(lost_events=0, lost_buffers=0, unresolved_product_functions=0,
        conservation_checks_passed=True, raw_gzip_sha256_match=True)
    window = load(EVIDENCE / ('checks/'+mode+'-window-audit.json'))
    assert window['valid'] is True and not window['overlapping_competing_processes']
    checks['exports'][mode]['etl_process_lifetimes_no_competing_overlap'] = True
reuse = load(EVIDENCE / 'hyperpath-reuse.json')
for name, item in reuse['files'].items():
    assert sha(ROOT / name) == item['sha256']
checks['hyperpath_capture_reused_byte_identically'] = True
stages = load(EVIDENCE / 'stages.json')
for name in ['warmup-legacy', 'legacy']:
    d = stages[name]['diagnostics']
    assert d['Passenger hyperpath lookup']['rebuilds'] == d['Passenger hyperpath lookup']['indexed_groups'] == 0
    assert d['Passenger hyperpath boarding filter']['indexed_walks'] == 0
    assert d['Passenger scheduler cause/migration']['packets'] == 0
checks['legacy_hyperpath_indexed_walks_and_rebuilds_zero'] = True
for suffix in ['', '-2']:
    assert load(WORK/('legacy-contaminated'+suffix)/'save-integrity.json') == load(WORK/'legacy/save-integrity.json')
checks['canonical_matches_both_rejected_runs'] = True
assert (WORK / 'wpr-recorder-complete.txt').is_file()
original = Path('C:/Users/bebe/.codex/worktrees/5a69/OpenTTD-patches/build-hyperpath-cpu/frozen/input.sav')
assert sha(original) == '306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629'
(EVIDENCE / 'checks/validation.json').write_text(json.dumps(checks, indent=2) + '\n', encoding='utf-8')
print(json.dumps(checks, indent=2))
