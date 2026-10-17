"""Warm Legacy from the original input for the same 8192 ticks as Hyperpath."""
from pathlib import Path
import hashlib
import json
import os
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
WORK = Path(__file__).resolve().parent
BUILD = ROOT / 'build-wpr-8462e6cd'
ORIGINAL = Path('C:/Users/bebe/.codex/worktrees/5a69/OpenTTD-patches/build-hyperpath-cpu/frozen')
PACKAGE = WORK / 'frozen-8192'
RUNNER = ROOT / 'utils/passenger_precomputed_real_map.py'


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def dump(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


env = dict(os.environ)
for key in ['OPENTTD_PASSENGER_TRACE', 'OPENTTD_PASSENGER_CHECKPOINT_TICK',
            'OPENTTD_PASSENGER_FOCUS_TRACE', 'OPENTTD_PASSENGER_DEPARTURE_DIAGNOSTIC',
            'OPENTTD_RETIREMENT_PROFILE', 'OPENTTD_RETIREMENT_REFERENCE']:
    env.pop(key, None)


def run(name, package, ticks, flags, checkpoint=False):
    command = [sys.executable, '-B', str(RUNNER), 'run', str(BUILD / 'Release/openttd.exe'),
        str(package), str(WORK / name), '--ticks', str(ticks), '--timeout', '2400',
        '--measurement-wait', '2400', '--main-thread-affinity-mask', '1', '--lang', str(BUILD / 'lang'), *flags]
    run_env = dict(env)
    if checkpoint:
        run_env['OPENTTD_PASSENGER_CHECKPOINT_TICK'] = '4096'
    print('Beginning ' + name, flush=True)
    with (WORK / (name + '-runner.log')).open('wb') as stream:
        subprocess.run(command, cwd=ROOT, env=run_env, stdout=stream, stderr=subprocess.STDOUT, check=True)
    meta = load(WORK / name / 'process.json')
    state = load(WORK / name / 'save-integrity.json')
    assert meta['exit_code'] == 0 and state['payments_valid'] is True
    assert not meta['contamination'] and not meta['phase_contamination']
    if ticks:
        clock = meta['actual-ticks.json']
        assert meta['simulation_timing_valid'] is True
        assert clock['end_tick'] - clock['start_tick'] == len(clock['samples']) == ticks
    print('Completed ' + name, flush=True)
    return meta, state


warm, seed = run('warmup', ORIGINAL, 8192, ['--precomputed', '--no-hyperpath',
    '--hyperpath-filter-mode', 'index', '--cold-precomputed', '--warm-precomputed', '--wait-background-ready'], checkpoint=True)
assert warm['actual-ticks.json']['start_tick'] == 3181119581 and seed['state_ticks'] == 3181127773
assert warm['hyperpath'] is False
with (WORK / 'freeze.log').open('wb') as stream:
    subprocess.run([sys.executable, '-B', str(RUNNER), 'freeze', str(WORK / 'warmup/result.sav'),
        str(ORIGINAL), str(BUILD), str(PACKAGE)], stdout=stream, stderr=subprocess.STDOUT, cwd=ROOT, check=True)
manifest = load(PACKAGE / 'package.json')
assert sha(PACKAGE / 'input.sav') == manifest['input_sha256'] == warm['result_sha256']
dump(WORK / 'frozen-seed.json', dict(product_commit='8462e6cd8280fe399fbb3b3e4a9cdb4413c935b4',
    mode='legacy', original_input_sha256=warm['input_sha256'], warmup_ticks=8192,
    start_tick=3181119581, frozen_tick=seed['state_ticks'], frozen_package=str(PACKAGE),
    frozen_input_sha256=manifest['input_sha256'], canonical=seed))
print('Frozen Legacy seed: ' + manifest['input_sha256'], flush=True)
reload, loaded = run('reload', PACKAGE, 0, ['--resume'])
differences = {k:dict(saved=seed.get(k), reloaded=loaded.get(k)) for k in seed.keys() | loaded.keys()
               if seed.get(k) != loaded.get(k)}
dump(WORK / 'reload-validation.json', dict(equal=not differences, fields=len(seed), differences=differences,
    frozen_tick=seed['state_ticks'], input_sha256=manifest['input_sha256']))
assert not differences, differences
os.chmod(PACKAGE / 'input.sav', stat.S_IREAD)
meta, final = run('legacy', PACKAGE, 2048, ['--resume', '--precomputed', '--no-hyperpath',
    '--warm-precomputed', '--wait-background-ready', '--hyperpath-filter-mode', 'index', '--wpr-profile'])
assert meta['actual-ticks.json']['start_tick'] == 3181127773 and final['state_ticks'] == 3181129821
assert meta['input_sha256'] == manifest['input_sha256']
assert meta['resume_settings'] is True and meta['cold_precomputed'] is False
assert meta['warm_complete'] is True and meta['background_ready']['completed'] is True
dump(WORK / 'measurement-complete.json', dict(ticks=2048, mode='legacy', input_sha256=manifest['input_sha256']))
print('Legacy steady-state WPR completed', flush=True)
