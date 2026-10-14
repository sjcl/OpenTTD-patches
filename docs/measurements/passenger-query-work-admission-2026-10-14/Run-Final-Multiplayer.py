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
try:
    os.write(lock, json.dumps({'pid': os.getpid(), 'task': 'Final Work admission multiplayer'}).encode())
    runner.wait_for_idle(14400, grace=20)
    subprocess.run([sys.executable, build / 'Exercise-Pruning-Multiplayer.py', build, build / a.directory,
        '--binary', binary.name, '--backend', 'periodic', '--input-save', a.input.resolve(), '--duration', str(a.duration)], check=True)
    result_path = build / a.directory / 'result.json'
    result = json.loads(result_path.read_text(encoding='utf-8'))
    result['binary_sha256'] = runner.sha256(binary)
    result['source_revision'] = 'c16da03990eff522af2abf0ca9379ce5d24e5aac'
    result['post_late_join_seconds'] = a.duration
    result_path.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
finally:
    os.close(lock)
    lock_path.unlink()
