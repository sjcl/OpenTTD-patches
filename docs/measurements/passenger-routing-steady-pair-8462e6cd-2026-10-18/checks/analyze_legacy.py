"""Compare independently warmed Legacy with the existing Hyperpath steady-input WPR."""
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
PREVIOUS_WORK = ROOT / 'build-passenger-steady-8462e6cd'
PREVIOUS = ROOT / 'docs/measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17'
EVIDENCE = ROOT / 'docs/measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18'
TOOLKIT = ROOT / 'build-wpr-profile/tools/wpt/Windows Kits/10/Windows Performance Toolkit'


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def dump(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def compress(source, target):
    with source.open('rb') as inp, target.open('wb') as out:
        with gzip.GzipFile(filename='', fileobj=out, mode='wb', mtime=0) as stream:
            shutil.copyfileobj(inp, stream)


def write_csv(path, rows):
    with path.open('w', encoding='utf-8', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)


def run(script, arguments, name):
    print('Running ' + script, flush=True)
    with (WORK / name).open('wb') as stream:
        subprocess.run([sys.executable, '-B', str(ROOT / script), *map(str, arguments)],
                       cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True)


def diagnostics(text):
    found = {}
    for line in text.splitlines():
        if not line.startswith('Passenger '):
            continue
        label, sep, body = line.partition(': ')
        if not sep:
            continue
        fields = {key:int(value) if value.isdigit() else value for key, value in re.findall(r'(\w+)=([^; ]+)', body)}
        if fields:
            found[label + ('/' + fields['cause'] if 'cause' in fields else '')] = fields
    return found


assert (WORK / 'measurement-complete.json').is_file()
assert (WORK / 'wpr-recorder-complete.txt').is_file()
EVIDENCE.mkdir(exist_ok=False)
(EVIDENCE / 'checks').mkdir()
(EVIDENCE / 'frozen-legacy').mkdir()
run('utils/passenger_wpr_export.py', [WORK, TOOLKIT, '--mode', 'legacy', '--output-name', 'wpa', '--local-symbols-only'], 'export.log')
run('utils/passenger_wpr_summarize.py', [WORK, EVIDENCE, '--export-name', 'wpa'], 'summarize.log')
summary = load(EVIDENCE / 'summary.json')
assert summary['hyperpath'] == load(PREVIOUS / 'summary.json')['hyperpath']
references = {}
for path in sorted((EVIDENCE / 'hyperpath').rglob('*')):
    if path.is_file():
        source = PREVIOUS / 'hyperpath' / path.relative_to(EVIDENCE / 'hyperpath')
        assert path.read_bytes() == source.read_bytes(), source
        references[source.relative_to(ROOT).as_posix()] = dict(sha256=sha(source), bytes=source.stat().st_size)
dump(EVIDENCE / 'hyperpath-reuse.json', dict(source_report_commit='9d7fba0adf390298b2f8c2cafa8acbd37b9d30bb',
    source_evidence=PREVIOUS.relative_to(ROOT).as_posix(), source_etl=str(PREVIOUS_WORK / 'hyperpath/cpu.etl'),
    source_etl_sha256=summary['hyperpath']['etl_sha256'],
    reused_summary_identical=True, files=references))

for name in ['warmup', 'reload']:
    target = EVIDENCE / name
    target.mkdir()
    for file in ['process.json', 'actual-ticks.json', 'save-integrity.json', 'transport-checkpoint.json']:
        if (WORK / name / file).is_file():
            shutil.copy2(WORK / name / file, target / file)
    for file in ['stdout.txt', 'stderr.txt', 'on-1-console.txt']:
        compress(WORK / name / file, target / (file + '.gz'))
    shutil.copytree(WORK / name / 'scripts', target / 'scripts')
for file in ['package.json', 'openttd.cfg']:
    shutil.copy2(WORK / 'frozen-8192' / file, EVIDENCE / 'frozen-legacy' / file)
for file in ['frozen-seed.json', 'reload-validation.json']:
    shutil.copy2(WORK / file, EVIDENCE / file)
for file in ['measure_legacy.py', 'retry_legacy.py', 'finalize_legacy.py', 'measurement-complete.json', 'analyze_legacy.py', 'validate_legacy.py', 'runner-before-wpr-phase-fix.py', 'wpr-recorder.log', 'wpr-recorder-complete.txt', 'freeze.log', 'rejected-retry-window.json']:
    shutil.copy2(WORK / file, EVIDENCE / 'checks' / file)
accepted_state = load(WORK / 'legacy/save-integrity.json')
retry_rows = []
for attempt, suffix in [(1, ''), (2, '-2')]:
    directory = WORK / ('legacy-contaminated'+suffix)
    rejected = EVIDENCE / ('checks/rejected-legacy-'+str(attempt))
    rejected.mkdir()
    for file in ['process.json', 'actual-ticks.json', 'on-1-console.txt']:
        compress(directory / file, rejected / (file+'.gz'))
    for file in ['save-integrity.json', 'etl-removed.json']:
        shutil.copy2(directory / file, rejected / file)
    shutil.copy2(WORK / ('wpr-recorder-contaminated'+suffix+'.log'), rejected / 'wpr-recorder.log')
    compress(WORK / ('legacy-contaminated'+suffix+'-runner.log'), rejected / 'runner.log.gz')
    rejected_state = load(directory / 'save-integrity.json')
    assert accepted_state == rejected_state
    retry_rows.extend(dict(attempt=attempt, field=key, rejected=rejected_state[key],
        accepted=accepted_state[key], equal=True) for key in sorted(accepted_state))
write_csv(EVIDENCE / 'legacy-retry-canonical.csv', retry_rows)
shutil.copy2(WORK / 'legacy/window-audit.json', EVIDENCE / 'checks/legacy-window-audit.json')
shutil.copy2(PREVIOUS_WORK / 'hyperpath/window-audit.json', EVIDENCE / 'checks/hyperpath-window-audit.json')
shutil.copy2(PREVIOUS / '.gitattributes', EVIDENCE / '.gitattributes')
shutil.copy2(WORK / 'cpu-sampled.wpaProfile', EVIDENCE / 'cpu-sampled.wpaProfile')
shutil.copy2(WORK / 'README.md', EVIDENCE / 'README.md')
(EVIDENCE / 'checks/tools').mkdir()
for file in ['passenger_precomputed_real_map.py', 'passenger_wpr_record.ps1', 'passenger_wpr_export.py', 'passenger_wpr_summarize.py', 'passenger_wpr_window.py']:
    shutil.copy2(ROOT / 'utils' / file, EVIDENCE / 'checks/tools' / file)

canonical = []
for mode, origin in [('legacy', WORK), ('hyperpath', PREVIOUS)]:
    saved, reloaded = [load(origin / phase / 'save-integrity.json') for phase in ['warmup', 'reload']]
    assert saved == reloaded and len(saved) == 24
    for key in sorted(saved):
        canonical.append(dict(mode=mode, field=key, saved=saved[key], reloaded=reloaded[key], equal=True))
write_csv(EVIDENCE / 'reload-canonical.csv', canonical)

stages = {}
causes = []
for name, directory, ticks in [('warmup-legacy', WORK/'warmup', 8192), ('legacy', WORK/'legacy', 2048),
                                ('hyperpath', WORK/'hyperpath', 2048)]:
    meta = load(directory / 'process.json')
    clock = meta['actual-ticks.json']
    assert meta['exit_code'] == 0 and meta['simulation_timing_valid']
    if name == 'legacy':
        assert all(o['stage'] == 'after_simulation' for o in meta['phase_contamination'])
    else:
        assert not meta['contamination'] and not meta['phase_contamination']
    assert clock['end_tick'] - clock['start_tick'] == len(clock['samples']) == ticks
    parsed = diagnostics((directory / 'on-1-console.txt').read_text(encoding='utf-8', errors='replace'))
    quartiles = []
    for offset in range(0, ticks, ticks // 4):
        samples = clock['samples'][offset:offset + ticks//4]
        quartiles.append(dict(ticks=len(samples), first_tick=samples[0][0], last_tick=samples[-1][0],
            simulation_wall_ms_per_tick=statistics.mean(s[1] for s in samples)/1000,
            journey_wall_ms_per_tick=statistics.mean(s[2] for s in samples)/1000))
    stages[name] = dict(start_tick=clock['start_tick'], end_tick=clock['end_tick'], ticks=ticks,
        input_sha256=meta['input_sha256'], result_sha256=meta['result_sha256'], diagnostics=parsed, quartiles=quartiles)
    for cause in ['new', 'expiry', 'failure', 'migration', 'generation']:
        values = parsed['Passenger scheduler cause/' + cause]
        causes.append(dict(run=name, ticks=ticks, cause=cause, packets=values['packets'],
            microseconds=values['microseconds'], wall_ms_per_tick=values['microseconds']/ticks/1000))
for name in ['warmup-legacy', 'legacy']:
    counts = stages[name]['diagnostics']
    assert counts['Passenger hyperpath lookup']['rebuilds'] == counts['Passenger hyperpath lookup']['indexed_groups'] == 0
    assert counts['Passenger hyperpath boarding filter']['indexed_walks'] == 0
    assert counts['Passenger scheduler cause/migration']['packets'] == 0
dump(EVIDENCE / 'stages.json', stages)
write_csv(EVIDENCE / 'scheduler-causes.csv', causes)

old = load(PREVIOUS / 'summary.json')['legacy']
comparisons = []
for label, item in [('legacy-from-hyperpath-seed', old), ('independently-warmed-legacy', summary['legacy'])]:
    comparisons.append(dict(run=label, start_tick=item['start_tick'], end_tick=item['end_tick'],
        simulation_wall_ms_per_tick=item['simulation_wall_ms_per_tick'],
        state_game_loop_cpu_ms_per_tick=item['sampled_cpu_ms_per_tick']['simulation_stack'],
        corrected_comparison_valid=False))
write_csv(EVIDENCE / 'legacy-input-comparison.csv', comparisons)

rows = list(csv.DictReader((EVIDENCE / 'function-diff.csv').open(encoding='utf-8')))
top = []
for scope in ['main', 'simulation_stack']:
    candidates = [row for row in rows if row['scope'] == scope]
    for field in ['delta_inclusive_ms_per_tick', 'delta_exclusive_ms_per_tick']:
        for direction, reverse in [('increase', True), ('decrease', False)]:
            for rank, row in enumerate(sorted(candidates, key=lambda item:float(item[field]), reverse=reverse)[:20], 1):
                top.append(dict(ranking=scope + '/' + field + '/' + direction, rank=rank, **row))
write_csv(EVIDENCE / 'top-function-differences.csv', top)
identity = load(PREVIOUS / 'build-identity.json')
assert sha(WORK / 'Release/openttd.exe') == identity['executable_sha256']
assert sha(WORK / 'Release/openttd.pdb') == identity['pdb_sha256']
dump(EVIDENCE / 'build-identity.json', dict(product_commit=identity['product_commit'], product_src_tree=identity['product_src_tree'],
    reused_build=True, previous_identity=PREVIOUS.relative_to(ROOT).as_posix()+'/build-identity.json',
    previous_identity_sha256=sha(PREVIOUS/'build-identity.json'), executable_sha256=identity['executable_sha256'],
    pdb_sha256=identity['pdb_sha256'], hardware=identity['hardware'], build_flags=identity['build_flags'],
    native_tests_previously_passed=identity['native_tests_previously_passed'],
    tool_scripts={item.relative_to(ROOT).as_posix():sha(item) for item in (EVIDENCE/'checks/tools').iterdir()}))
print(json.dumps(dict(seed=load(WORK/'frozen-seed.json')['frozen_input_sha256'],
    legacy_lookup=stages['legacy']['diagnostics']['Passenger hyperpath lookup'],
    state_game_loop_cpu={mode:summary[mode]['sampled_cpu_ms_per_tick']['simulation_stack'] for mode in ['legacy','hyperpath']}), indent=2), flush=True)
