"""Load an actual save, inspect immutable Packet state, and save in current v3."""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import shutil
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('runner', HERE.parent / 'passenger-routing-profile-2026-10-13/Run-Profile.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
p = argparse.ArgumentParser(description=__doc__)
for name in ['binary', 'package', 'input', 'output']:
    p.add_argument('--' + name, type=pathlib.Path, required=True)
p.add_argument('--ticks', type=int, default=0)
p.add_argument('--no-integrity', action='store_true', help='Reference old binary lacks the read-only inspector')
a = p.parse_args()
directory = a.output.resolve()
directory.mkdir(exist_ok=False)
for name in runner.ASSET_DIRS:
    if (a.package / name).is_dir():
        shutil.copytree(a.package / name, directory / name, copy_function=runner.hardlink_or_copy)
shutil.copy2(a.package / 'openttd.cfg', directory / 'openttd.cfg')
shutil.copy2(a.input, directory / 'input.sav')
shutil.copy2(a.binary, directory / 'openttd.exe')
scripts = directory / 'scripts'
scripts.mkdir(exist_ok=True)
log = (directory / 'on-1-console.txt').as_posix()
result = (directory / 'result').as_posix()
start = f'script {log}\ngetfulldate\npassenger_journey_stats inventory\n'
inspect = '' if a.no_integrity else 'passenger_journey_stats save_integrity\n'
finish = f'script {log}\ngetfulldate\npassenger_journey_stats inventory\n{inspect}save {result}\nscript\n'
if a.ticks:
    start += 'unpause\nscript\n'
else:
    start += finish + 'quit\n'
(scripts / 'game_start.scr').write_text(start, encoding='utf-8')
(scripts / 'passenger_benchmark_end.scr').write_text(finish, encoding='utf-8')
driver = f'null:ticks={a.ticks}' if a.ticks else 'null:until_exit'
with (directory / 'stdout.txt').open('w', encoding='utf-8') as out, (directory / 'stderr.txt').open('w', encoding='utf-8') as err:
    completed = subprocess.run([str(directory / 'openttd.exe'), '-x', '-c', 'openttd.cfg', '-g', 'input.sav',
        '-v', driver, '-snull', '-mnull', '-d', 'sl=2'], cwd=directory, stdout=out, stderr=err, timeout=240)
if completed.returncode or not (directory / 'result.sav').is_file() or (not a.no_integrity and not (directory / 'save-integrity.json').is_file()):
    raise RuntimeError(f'Save stage failed: {completed.returncode}; see {directory}')
metadata = {'input_sha256': runner.sha256(directory / 'input.sav'), 'binary_sha256': runner.sha256(directory / 'openttd.exe'),
    'result_sha256': runner.sha256(directory / 'result.sav'), 'result_bytes': (directory / 'result.sav').stat().st_size,
    'exit_code': completed.returncode, 'requested_ticks': a.ticks,
    'integrity': None if a.no_integrity else json.loads((directory / 'save-integrity.json').read_text(encoding='utf-8'))}
if (directory / 'continuation-driver.json').is_file():
    metadata['actual_clock'] = json.loads((directory / 'continuation-driver.json').read_text(encoding='utf-8'))
(directory / 'stage.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
print(json.dumps(metadata, indent=2))
