"""Serial, fresh-process real-map collection with audited WPR jobs."""
from prepare import ROOT, WORK, EVIDENCE, dump
from pathlib import Path
import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time

TOOLKIT = Path('C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-wpr-9ab0228d/tools/wpt/Windows Kits/10/Windows Performance Toolkit')
sys.path.insert(0, str(ROOT / 'utils'))
from passenger_wpr_window import audit_window

def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))

def quality(directory, mode):
    marks = (directory / 'recorder-markers.txt').read_text(encoding='utf-8-sig')
    points = {name: int(stamp) for stamp, name in re.findall(r'Mark,\s*(\d+),\s*' + mode + r'-(begin|end)', marks)}
    assert points['begin'] < points['end']
    stats = (directory / 'trace-stats.txt').read_text(encoding='utf-8-sig')
    assert re.search(r'Total # Lost Events\s+: 0\b', stats)
    assert re.search(r'Total # Lost Buffers\s+: 0\b', stats)
    return {'valid': True, 'lost_events': 0, 'lost_buffers': 0, 'markers': points}

def launch(label, mode, output, ticks, *, wpr=False, fixture=False, input_save=None, validate=False, workers=4):
    assert not output.exists(), output
    artifacts = WORK / 'artifacts-v2' / label
    command = [sys.executable, '-B', str(WORK / 'real_runner.py'), 'run', str(artifacts / 'Release/openttd.exe'),
        str(WORK / 'input'), str(output), '--ticks', str(ticks), '--timeout', '1800', '--measurement-wait', '600',
        '--resume', '--wait-saved-replay', '--precomputed', '--hyperpath' if mode == 'hyperpath' else '--no-hyperpath',
        '--warm-precomputed', '--wait-background-ready', '--main-thread-affinity-mask', '1', '--lang', str(artifacts / 'lang')]
    if ticks: command += ['--hyperpath-filter-mode', 'validate' if validate else 'index']
    if input_save: command += ['--input', str(input_save)]
    if wpr: command += ['--wpr-profile']
    if validate: command += ['--validation-only']
    env = {k: v for k, v in os.environ.items() if not k.startswith('OPENTTD_')}
    env['OPENTTD_REAL_WORKERS'] = str(workers)
    if fixture: env['OPENTTD_REAL_FIXTURE'] = '1'
    log = output.parent / (output.name + '-runner.log')
    log.parent.mkdir(parents=True, exist_ok=True)
    dump(log.with_suffix('.command.json'), {'command': command, 'env': {k: v for k, v in env.items() if k.startswith('OPENTTD_')}})
    print(f'Start {label} {mode} ticks={ticks} fixture={fixture} replay={bool(input_save)}', flush=True)
    with log.open('wb') as stream:
        child = subprocess.Popen(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT)
        if wpr:
            until = time.monotonic() + 120
            while not output.exists():
                if child.poll() is not None: raise RuntimeError(f'Runner failed before setup: {log}')
                if time.monotonic() > until: raise TimeoutError('Runner output creation')
                time.sleep(.1)
            key = '-'.join(output.relative_to(WORK).parts)
            job = WORK / 'jobs' / key
            job.mkdir()
            dump(job / 'job.json', {'mode': mode, 'directory': output.relative_to(WORK).as_posix()})
        code = child.wait()
    if code != 0: raise RuntimeError(f'Run failed ({code}): {log}')
    data = read(output / 'process.json')
    assert data['exit_code'] == 0 and data['warm_complete'] and data['saved_replay']['ready']
    assert data['background_ready']['completed']
    assert data['background_ready']['tick_before'] == data['background_ready']['tick_after'] == 3201034745
    assert not data['phase_contamination'], 'Preparation contaminated; not valid for preparation timings'
    if wpr:
        assert read(job / 'done.json')['valid']
        q = quality(output, mode)
        window = audit_window(output, TOOLKIT, mode)
        dump(output / 'adoption.json', {'quality': q, 'window_audit_return': window, 'accepted': True})
    console = (output / 'on-1-console.txt').read_text(encoding='utf-8')
    assert 'Preparation background wait: complete=true' in console
    for row in re.findall(r'Preparation probe: ([^\n]+)', console):
        fields = dict(piece.split('=', 1) for piece in row.split('; '))
        assert fields['ready'] == 'true' and fields['tick_before'] == fields['tick_after'] == '3201034745'
    dump(output / 'preparation-probes.json', [dict(piece.split('=', 1) for piece in row.split('; '))
        for row in re.findall(r'Preparation probe: ([^\n]+)', console)])
    print(f'Accepted {label} {mode}: {output}', flush=True)

def normal():
    assert quality(WORK / 'pilot', 'pilot')['valid']
    for label, name in [('baseline', 'A-baseline'), ('candidate', 'B-candidate')]:
        root = WORK / 'runs' / name
        root.mkdir(parents=True, exist_ok=True)
        for part in ['Release', 'lang']:
            shutil.copytree(WORK / 'artifacts-v2' / label / part, root / part)
        shutil.copy2(WORK / 'artifacts-v2' / label / 'cpu-sampled.wpaProfile', root / 'cpu-sampled.wpaProfile')
        for mode in ['legacy', 'hyperpath']:
            launch(label, mode, root / mode, 2048, wpr=True)

def fixtures():
    for mode in ['legacy', 'hyperpath']:
        output = WORK / 'fixtures' / mode
        launch('candidate', mode, output, 0, fixture=True)
        console = (output / 'on-1-console.txt').read_text(encoding='utf-8')
        assert re.search(r'Mixed replay fixture: ready=true; completed_rows=[01];', console)
        data = read(output / 'save-integrity.json')
        assert data['completed_rows'] <= 1 and data['retained_rows'] > 0 and data['table_operations'] > 0

def replay():
    for label in ['baseline', 'candidate']:
        for mode in ['legacy', 'hyperpath']:
            launch(label, mode, WORK / 'replay' / label / mode, 0,
                input_save=WORK / 'fixtures' / mode / 'result.sav')

def validate():
    launch('candidate', 'hyperpath', WORK / 'validate' / 'hyperpath', 2048, validate=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('phase', choices=['normal', 'fixtures', 'replay', 'validate'])
    args = parser.parse_args()
    globals()[args.phase]()
