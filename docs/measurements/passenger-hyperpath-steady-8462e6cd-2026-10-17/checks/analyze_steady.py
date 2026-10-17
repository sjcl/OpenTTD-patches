"""Package steady-seed provenance and sampled CPU results, without old-input normalization."""
from pathlib import Path
import csv
import gzip
import hashlib
import json
import re
import shutil
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
WORK = Path(__file__).resolve().parent
EVIDENCE = ROOT / 'docs/measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17'
OLD = ROOT / 'docs/measurements/passenger-hyperpath-index-8462e6cd-2026-10-17'
TOOLKIT = ROOT / 'build-wpr-profile/tools/wpt/Windows Kits/10/Windows Performance Toolkit'


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def compress(source, target):
    with source.open('rb') as inp, target.open('wb') as out:
        with gzip.GzipFile(filename='', fileobj=out, mode='wb', mtime=0) as stream:
            shutil.copyfileobj(inp, stream)


def csv_write(path, rows):
    with path.open('w', encoding='utf-8', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)


def command(script, args, log):
    print('Running ' + script, flush=True)
    with log.open('wb') as stream:
        subprocess.run([sys.executable, '-B', str(ROOT / script), *map(str, args)],
                       cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True)


EVIDENCE.mkdir(exist_ok=False)
(EVIDENCE / 'checks').mkdir()
(EVIDENCE / 'frozen').mkdir()
command('utils/passenger_wpr_export.py', [WORK, TOOLKIT, '--output-name', 'wpa', '--local-symbols-only'], WORK / 'export.log')
command('utils/passenger_wpr_summarize.py', [WORK, EVIDENCE, '--export-name', 'wpa'], WORK / 'summarize.log')

for name in ['warmup', 'reload']:
    output = EVIDENCE / name
    output.mkdir()
    for file in ['process.json', 'save-integrity.json', 'actual-ticks.json', 'transport-checkpoint.json']:
        if (WORK / name / file).is_file():
            shutil.copy2(WORK / name / file, output / file)
    for file in ['stdout.txt', 'stderr.txt', 'on-1-console.txt']:
        compress(WORK / name / file, output / (file + '.gz'))
    shutil.copytree(WORK / name / 'scripts', output / 'scripts')
for file in ['package.json', 'openttd.cfg']:
    shutil.copy2(WORK / 'frozen-8192' / file, EVIDENCE / 'frozen' / file)
for file in ['frozen-seed.json', 'reload-validation.json']:
    shutil.copy2(WORK / file, EVIDENCE / file)
for file in ['wpr-recorder.log', 'wpr-recorder-complete.txt', 'freeze.log', 'measure_steady.py', 'retry_pair.py', 'restart_recorder.ps1', 'analyze_steady.py', 'validate_steady.py']:
    shutil.copy2(WORK / file, EVIDENCE / 'checks' / file)
shutil.copy2(WORK / 'README.md', EVIDENCE / 'README.md')
rejected = EVIDENCE / 'checks/rejected-legacy'
rejected.mkdir()
for file in ['process.json', 'actual-ticks.json', 'on-1-console.txt']:
    compress(WORK / 'legacy-contaminated' / file, rejected / (file + '.gz'))
shutil.copy2(WORK / 'legacy-contaminated/save-integrity.json', rejected / 'save-integrity.json')
shutil.copy2(WORK / 'wpr-recorder-contaminated.log', rejected / 'wpr-recorder.log')
compress(WORK / 'legacy-contaminated-runner.log', rejected / 'runner.log.gz')
shutil.copy2(WORK / 'cpu-sampled.wpaProfile', EVIDENCE / 'cpu-sampled.wpaProfile')
shutil.copy2(OLD / '.gitattributes', EVIDENCE / '.gitattributes')

seed = load(WORK / 'warmup/save-integrity.json')
reload = load(WORK / 'reload/save-integrity.json')
assert seed == reload and len(seed) == 24
csv_write(EVIDENCE / 'reload-canonical.csv', [dict(field=key, saved=seed[key], reloaded=reload[key], equal=True) for key in sorted(seed)])
accepted_state = load(WORK / 'legacy/save-integrity.json')
rejected_state = load(WORK / 'legacy-contaminated/save-integrity.json')
assert accepted_state == rejected_state
csv_write(EVIDENCE / 'legacy-retry-canonical.csv', [dict(field=key, rejected=rejected_state[key], accepted=accepted_state[key], equal=True) for key in sorted(accepted_state)])
package = load(WORK / 'frozen-8192/package.json')
assert sha(WORK / 'frozen-8192/input.sav') == package['input_sha256']

def console_stats(text):
    result = {}
    for line in text.splitlines():
        if not line.startswith('Passenger '):
            continue
        label, sep, data = line.partition(': ')
        if not sep:
            continue
        fields = {key: int(value) if value.isdigit() else value
                  for key, value in re.findall(r'(\w+)=([^; ]+)', data)}
        if 'cause' in fields:
            label += '/' + fields['cause']
        if fields:
            result[label] = fields
    return result


stages = {}
migration_rows = []
for name, ticks in [('warmup', 8192), ('legacy', 2048), ('hyperpath', 2048)]:
    meta = load(WORK / name / 'process.json')
    clock = meta['actual-ticks.json']
    assert meta['exit_code'] == 0 and meta['simulation_timing_valid']
    assert not meta['contamination'] and not meta['phase_contamination']
    assert clock['end_tick'] - clock['start_tick'] == len(clock['samples']) == ticks
    assert meta['save-integrity.json']['payments_valid'] is True
    text = (WORK / name / 'on-1-console.txt').read_text(encoding='utf-8', errors='replace')
    stats = console_stats(text)
    quartiles = []
    for offset in range(0, ticks, ticks // 4):
        samples = clock['samples'][offset:offset + ticks // 4]
        quartiles.append(dict(first_tick=samples[0][0], last_tick=samples[-1][0], ticks=len(samples),
            simulation_wall_ms_per_tick=statistics.mean(s[1] for s in samples)/1000,
            journey_wall_ms_per_tick=statistics.mean(s[2] for s in samples)/1000))
    stages[name] = dict(start_tick=clock['start_tick'], end_tick=clock['end_tick'], ticks=ticks,
        diagnostics=stats, time_quartiles=quartiles,
        input_sha256=meta['input_sha256'], result_sha256=meta['result_sha256'])
    for cause in ['new', 'expiry', 'failure', 'migration', 'generation']:
        values = stats['Passenger scheduler cause/' + cause]
        migration_rows.append(dict(run=name, input='original' if name=='warmup' else '8192-tick seed',
            ticks=ticks, cause=cause, packets=values['packets'], packets_per_tick=values['packets']/ticks,
            microseconds=values['microseconds'], wall_ms_per_tick=values['microseconds']/ticks/1000))
    if name != 'warmup':
        assert meta['resume_settings'] is True and meta['cold_precomputed'] is False
        assert meta['input_sha256'] == package['input_sha256']
        assert meta['warm_complete'] is True and meta['background_ready']['completed'] is True
        assert clock['start_tick'] == seed['state_ticks']
        assert stats['Passenger hyperpath lookup']['violations'] == 0
old_console = gzip.decompress((OLD / 'hyperpath/on-1-console.txt.gz').read_bytes()).decode('utf-8', errors='replace')
for cause in ['new', 'expiry', 'failure', 'migration', 'generation']:
    values = console_stats(old_console)['Passenger scheduler cause/' + cause]
    migration_rows.append(dict(run='previous-hyperpath', input='original', ticks=2048, cause=cause,
        packets=values['packets'], packets_per_tick=values['packets']/2048,
        microseconds=values['microseconds'], wall_ms_per_tick=values['microseconds']/2048/1000))
csv_write(EVIDENCE / 'scheduler-causes.csv', migration_rows)
dump(EVIDENCE / 'stages.json', stages)

# The old-input run is descriptive context only. Its tick interval and work are different.
old_summary, summary = load(OLD / 'summary.json'), load(EVIDENCE / 'summary.json')
comparison = []
for mode in ['legacy', 'hyperpath']:
    for label, data in [('previous-original-input', old_summary[mode]), ('new-8192-tick-input', summary[mode])]:
        comparison.append(dict(mode=mode, run=label, start_tick=data['start_tick'], end_tick=data['end_tick'],
            simulation_wall_ms_per_tick=data['simulation_wall_ms_per_tick'],
            sampled_state_game_loop_cpu_ms_per_tick=data['sampled_cpu_ms_per_tick']['simulation_stack'],
            corrected_comparison_valid=False))
csv_write(EVIDENCE / 'input-window-context.csv', comparison)

functions = list(csv.DictReader((EVIDENCE / 'function-diff.csv').open(encoding='utf-8')))
top = []
for scope in ['main', 'simulation_stack']:
    candidates = [row for row in functions if row['scope'] == scope]
    for field in ['delta_inclusive_ms_per_tick', 'delta_exclusive_ms_per_tick']:
        for sign, reverse in [('increase', True), ('decrease', False)]:
            for rank, row in enumerate(sorted(candidates, key=lambda r: float(r[field]), reverse=reverse)[:20], 1):
                top.append(dict(ranking=scope + '/' + field + '/' + sign, rank=rank, **row))
csv_write(EVIDENCE / 'top-function-differences.csv', top)

identity = load(OLD / 'build-identity.json')
dump(EVIDENCE / 'build-identity.json', dict(product_commit=identity['commit'],
    product_src_tree=identity['product_src_tree'], reused_build=True,
    previous_evidence=OLD.relative_to(ROOT).as_posix(),
    previous_identity_sha256=sha(OLD / 'build-identity.json'),
    executable_sha256=sha(WORK / 'Release/openttd.exe'), pdb_sha256=sha(WORK / 'Release/openttd.pdb'),
    hardware=identity['hardware'], build_flags=identity['build_flags'],
    native_tests_previously_passed=identity['native_tests'],
    tool_scripts={item.relative_to(ROOT).as_posix():sha(item) for item in
        [ROOT/'utils/passenger_precomputed_real_map.py', ROOT/'utils/passenger_wpr_record.ps1',
         ROOT/'utils/passenger_wpr_export.py', ROOT/'utils/passenger_wpr_summarize.py']}))
assert sha(WORK / 'Release/openttd.exe') == identity['files']['Release/openttd.exe']['sha256']
assert sha(WORK / 'Release/openttd.pdb') == identity['files']['Release/openttd.pdb']['sha256']
print(json.dumps(dict(seed=package['input_sha256'], reload_equal=True,
    migration=stages['hyperpath']['diagnostics']['Passenger scheduler cause/migration'],
    simulation_cpu={mode:summary[mode]['sampled_cpu_ms_per_tick']['simulation_stack'] for mode in ['legacy','hyperpath']}), indent=2), flush=True)
