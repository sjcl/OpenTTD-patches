from pathlib import Path
import subprocess, sys, json
from concurrent.futures import ThreadPoolExecutor
ROOT = Path(r'C:\Users\bebe\.codex\worktrees\6d2b\OpenTTD-patches')
BASE = ROOT / 'build-clang-lto-comparison'
def run(case):
    index, (server, client) = case
    name = f'mp-{server}-{client}'
    output = BASE / name
    command = [sys.executable, str(ROOT / 'utils/passenger_precomputed_join.py'),
        str(BASE / server / 'Release/openttd.exe'), str(BASE / 'input'), str(BASE / 'input/input.sav'), str(output), str(BASE / server / 'lang'),
        '--client-binary', str(BASE / client / 'Release/openttd.exe'), '--port', str(17760+index),
        '--ticks', '256', '--frame-freq', '1', '--observe-timeout', '600', '--server-workers', '4', '--client-workers', '4', '--wait-saved-replay', '--wait-server-ready']
    print('START', name, flush=True)
    with (BASE / (name+'.log')).open('wb') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    record = json.loads((output / 'result.json').read_text())
    print('DONE', name, 'pass', record['pass'], 'ticks', record.get('actual_ticks'), 'error', record.get('error'), flush=True)
    if result.returncode or not record['pass']:
        raise RuntimeError(f'Cross-compiler multiplayer failed: {output}')
with ThreadPoolExecutor(max_workers=2) as pool:
    list(pool.map(run, enumerate([('thin', 'full'), ('full', 'thin')])))
