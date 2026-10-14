"""Finish cold-cache, saved continuation, real-map joins and product checks serially."""
import argparse
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
spec = importlib.util.spec_from_file_location("runner", HERE.parent / "passenger-routing-profile-2026-10-13/Run-Profile.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("build", type=pathlib.Path)
p.add_argument("package", type=pathlib.Path)
p.add_argument("dependencies", type=pathlib.Path)
a = p.parse_args()
build, package = a.build.resolve(), a.package.resolve()
checkpoint = build / "final-measured/q16-t49152/on-1/on-1-result.sav"
sequence = build / "final-measured/sequence.json"
deadline = time.monotonic() + 7200
while not (sequence.exists() and json.loads(sequence.read_text(encoding='utf-8'))['status'] == 'complete'):
    if time.monotonic() > deadline:
        raise TimeoutError("Final primary run did not complete")
    time.sleep(5)

def command(args, log):
    with (build / log).open('w', encoding='utf-8') as output:
        subprocess.run(list(map(str, args)), cwd=ROOT, stdout=output, stderr=subprocess.STDOUT,
                       env={**os.environ, 'MSBUILDDISABLENODEREUSE': '1'}, check=True)

lock_path = pathlib.Path(tempfile.gettempdir()) / 'openttd-passenger-routing-measurement.lock'
lock = runner.acquire_measurement_lock(lock_path, 7200)
try:
    os.write(lock, json.dumps({'pid': os.getpid(), 'task': 'Admission final checks'}).encode())
    binary = build / 'openttd-q16-t49152.exe'
    metadata = json.loads(binary.with_suffix('.instrumentation.json').read_text(encoding='utf-8'))
    args = SimpleNamespace(package=package, binary=binary, build=build, output=build / 'final-cold/q16-t49152',
        label='q16-t49152', revision=metadata['source_revision'], kind='minimal', ticks=8192,
        phase='initial_8192', repetitions=1, modes=['cold'], cache_off_scope='all', resume_settings=False,
        wait_idle_seconds=7200, idle_grace_seconds=90)
    runner.run_locked(args)
    print('Cold completed', flush=True)
    runner.wait_for_idle(7200, grace=20)
    command([sys.executable, '-B', HERE.parent / 'passenger-timed-anytime-2026-10-14/Prepare-Continuation.py', ROOT, build], 'prepare-continuation.log')
    command(['cmake', '--build', build, '--config', 'Release', '--target', 'openttd', '--parallel', '8'], 'build-continuation.log')
    continuation = build / 'openttd-q16-continuation.exe'
    shutil.copy2(build / 'Release/openttd.exe', continuation)
    driver = json.loads((build / 'journey-profile/instrumentation.json').read_text(encoding='utf-8'))['state_tick_driver']
    continuation.with_suffix('.instrumentation.json').write_text(json.dumps({**metadata, 'state_tick_driver': driver,
        'binary_sha256': runner.sha256(continuation)}, indent=2) + '\n', encoding='utf-8')
    resumed = build / 'continuation-package'
    resumed.mkdir(exist_ok=False)
    for name in runner.ASSET_DIRS:
        if (package / name).exists():
            shutil.copytree(package / name, resumed / name, copy_function=runner.hardlink_or_copy)
    shutil.copy2(checkpoint, resumed / 'input.sav')
    shutil.copy2(package / 'openttd.cfg', resumed / 'openttd.cfg')
    package_metadata = json.loads((package / 'package.json').read_text(encoding='utf-8'))
    package_metadata['input_sha256'] = runner.sha256(checkpoint)
    runner.dump(resumed / 'package.json', package_metadata)
    args.package, args.binary, args.output = resumed, continuation, build / 'final-continuation/q16-continuation'
    args.label, args.modes, args.phase, args.resume_settings = 'q16-continuation', ['on'], 'continuation_8192', True
    runner.run_locked(args)
    print('Continuation completed', flush=True)
    runner.wait_for_idle(7200, grace=20)
    command([runner.POWERSHELL, '-NoProfile', '-File', HERE.parent / 'passenger-timed-anytime-2026-10-14/Build-Variants.ps1',
             '-Source', ROOT, '-Build', build, '-Dependencies', a.dependencies.resolve(), '-Modes', 'network'], 'build-network.log')
finally:
    os.close(lock)
    lock_path.unlink()

command([sys.executable, '-B', HERE.parent / 'passenger-timed-anytime-2026-10-14/Verify-Final.py', build, checkpoint,
         a.dependencies.resolve(), '--multiplayer-input', package / 'input.sav', '--multiplayer-directory', 'mp-admission'], 'verify-product.log')
print('Multiplayer and product completed', flush=True)
