"""Run the two-client real-map join test under the shared host reservation."""
import argparse
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime, timezone

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('runner', HERE.parent / 'passenger-routing-profile-2026-10-13/Run-Profile.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=pathlib.Path)
p.add_argument('input', type=pathlib.Path)
p.add_argument('--duration', type=int, default=120)
p.add_argument('--directory', default='mp-final-review')
a = p.parse_args()
build = a.build.resolve()
binary = build / 'openttd-network-final.exe'
shutil.copy2(build / 'Release/openttd.exe', binary)
data = bytearray(binary.read_bytes())
pe = int.from_bytes(data[0x3c:0x40], 'little')
offset = pe + 24 + 68
if int.from_bytes(data[offset:offset + 2], 'little') != 2:
    raise ValueError('Expected GUI subsystem before console conversion')
data[offset:offset + 2] = (3).to_bytes(2, 'little')
binary.write_bytes(data)
lock_path = pathlib.Path(tempfile.gettempdir()) / 'openttd-passenger-routing-measurement.lock'
lock = runner.acquire_measurement_lock(lock_path, 14400)
metadata = {'binary_sha256': runner.sha256(binary), 'input_sha256': runner.sha256(a.input.resolve()),
    'source_revision': subprocess.check_output(['git', '-C', HERE.parents[2], 'rev-parse', 'HEAD'], text=True).strip(),
    'requested_post_join_seconds': a.duration, 'contamination': [], 'outcome': 'not_started'}
run_directory = build / a.directory
stopped = threading.Event()
monitor = None
def observe_contention():
    while not stopped.wait(5):
        try:
            pids = json.loads((run_directory / 'owned-pids.json').read_text(encoding='utf-8')) if (run_directory / 'owned-pids.json').exists() else []
            busy = [entry for entry in runner.competing_processes() if int(entry.rsplit(':', 1)[1]) not in pids]
            if busy:
                metadata['contamination'].append({'utc': datetime.now(timezone.utc).isoformat(), 'processes': busy})
        except Exception as error:
            metadata['contamination'].append({'monitor_error': str(error)})
try:
    os.write(lock, json.dumps({'pid': os.getpid(), 'task': 'Final Work admission multiplayer'}).encode())
    runner.wait_for_idle(14400, grace=20)
    metadata['started_utc'] = datetime.now(timezone.utc).isoformat()
    metadata['outcome'] = 'running'
    monitor = threading.Thread(target=observe_contention)
    monitor.start()
    subprocess.run([sys.executable, build / 'Exercise-Pruning-Multiplayer.py', build, build / a.directory,
        '--binary', binary.name, '--backend', 'periodic', '--input-save', a.input.resolve(), '--duration', str(a.duration)], check=True)
    result_path = build / a.directory / 'result.json'
    result = json.loads(result_path.read_text(encoding='utf-8'))
    result['binary_sha256'] = runner.sha256(binary)
    result['source_revision'] = 'c16da03990eff522af2abf0ca9379ce5d24e5aac'
    result['post_late_join_seconds'] = a.duration
    result_path.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    metadata['outcome'] = 'pass'
except BaseException as error:
    metadata['outcome'] = 'fail'
    metadata['error'] = str(error)
    raise
finally:
    stopped.set()
    if monitor is not None:
        monitor.join()
    metadata['finished_utc'] = datetime.now(timezone.utc).isoformat()
    (run_directory / 'run-metadata.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    os.close(lock)
    lock_path.unlink()
