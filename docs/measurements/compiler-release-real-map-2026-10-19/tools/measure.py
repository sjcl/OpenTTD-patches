from pathlib import Path
import subprocess, sys, json, hashlib, platform, statistics, re
ROOT = Path(r'C:\Users\bebe\.codex\worktrees\6d2b\OpenTTD-patches')
BASE = ROOT / 'build-compiler-comparison'
PY = sys.executable
def run(compiler, name, ticks, wpr=False):
    output = BASE / 'runs' / name
    if (output / 'process.json').exists():
        saved = json.loads((output / 'process.json').read_text())
        if saved.get('simulation_timing_valid') and saved.get('warm_complete') and saved.get('result_sha256'):
            assert saved['binary_sha256'] == hashlib.sha256((BASE / compiler / 'Release/openttd.exe').read_bytes()).hexdigest()
            print('REUSE', name, flush=True)
            return
    command = [PY, str(ROOT / 'utils/passenger_precomputed_real_map.py'), 'run',
               str(BASE / compiler / 'Release/openttd.exe'), str(BASE / 'input'), str(output),
               '--lang', str(BASE / compiler / 'lang'), '--ticks', str(ticks), '--timeout', '900',
               '--resume', '--wait-saved-replay', '--precomputed', '--cold-precomputed', '--warm-precomputed']
    if wpr:
        command += ['--wpr-profile']
    print('START', name, flush=True)
    with (BASE / (name + '.log')).open('w', encoding='utf-8') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    record = json.loads((output / 'process.json').read_text())
    assert record['simulation_timing_valid'] and record['warm_complete'] and not record['load_failed'], record
    assert record['save-integrity.json']['payments_valid']
    print('DONE', name, 'CPU', record.get('process_cpu_seconds'), 'ticks', record['actual-ticks.json']['end_tick'] - record['actual-ticks.json']['start_tick'], flush=True)
if sys.argv[1] == 'smoke':
    run('msvc', 'smoke2-msvc', 32)
    run('clang', 'smoke2-clang', 32)
else:
    for compiler, index in [('msvc', 1), ('clang', 1), ('clang', 2), ('msvc', 2)]:
        run(compiler, f'wpr-{compiler}-{index}', 1024, wpr=True)
