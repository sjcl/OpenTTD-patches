"""After profiling, check actual checkpoint reload and cache/scratch continuation."""
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
ROOT = HERE.parents[2]
spec = importlib.util.spec_from_file_location('runner', HERE.parent / 'passenger-routing-profile-2026-10-13/Run-Profile.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=pathlib.Path)
p.add_argument('package', type=pathlib.Path)
p.add_argument('checkpoint', type=pathlib.Path)
p.add_argument('--directory', default='integration-2026-10-15-saveload')
a = p.parse_args()
build, package = a.build.resolve(), a.package.resolve()
out = build / a.directory
out.mkdir(exist_ok=False)
lock_path = pathlib.Path(tempfile.gettempdir()) / 'openttd-passenger-routing-measurement.lock'
lock = runner.acquire_measurement_lock(lock_path, 14400)

def run(command, log):
    with (out / log).open('w', encoding='utf-8') as stream:
        subprocess.run([str(v) for v in command], cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True)

try:
    os.write(lock, json.dumps({'pid': os.getpid(), 'task': 'Merged Work admission Save/Load'}).encode())
    runner.wait_for_idle(14400, grace=20)
    run(['cmake', '-S', ROOT, '-B', build, '-DOPTION_PASSENGER_BENCHMARK=ON'], 'configure.log')
    run([sys.executable, HERE / 'Prepare-Save-Integrity.py', build], 'prepare-integrity.log')
    run([sys.executable, HERE.parent / 'passenger-timed-anytime-2026-10-14/Prepare-Continuation.py', ROOT, build], 'prepare-driver.log')
    run(['cmake', '--build', build, '--config', 'Release', '--target', 'openttd', '--parallel', '8'], 'build.log')
    binary = out / 'openttd-save-inspector.exe'
    shutil.copy2(build / 'Release/openttd.exe', binary)
    stages = {}
    for name, input_path, ticks, extra in [
        ('load', a.checkpoint.resolve(), 0, []),
        ('reload', out / 'load/result.sav', 0, []),
        ('warm-512', out / 'load/result.sav', 512, []),
        ('cold-scratch-off-512', out / 'load/result.sav', 512, ['--cache-mode', 'cold', '--scratch-off']),
        ('continued-reload', out / 'warm-512/result.sav', 0, []),
    ]:
        runner.wait_for_idle(14400, grace=20)
        run([sys.executable, HERE / 'Run-Save-Stage.py', '--binary', binary, '--package', package,
             '--input', input_path, '--output', out / name, '--ticks', ticks, *extra], name + '.log')
        stages[name] = json.loads((out / name / 'stage.json').read_text(encoding='utf-8'))
    keys = ['packets', 'cargo_units', 'known_od_units', 'journey_packets', 'packet_state_digest', 'payment_digest', 'payments_valid']
    checks = {'load_payments_valid': stages['load']['integrity']['payments_valid'],
              'zero_tick_reload_equal': all(stages['load']['integrity'][k] == stages['reload']['integrity'][k] for k in keys),
              'continued_reload_equal': all(stages['warm-512']['integrity'][k] == stages['continued-reload']['integrity'][k] for k in keys),
              'cache_scratch_full_save_equal': stages['warm-512']['result_sha256'] == stages['cold-scratch-off-512']['result_sha256'],
              'cache_scratch_packet_payment_equal': all(stages['warm-512']['integrity'][k] == stages['cold-scratch-off-512']['integrity'][k] for k in keys)}
    for name in ['warm-512', 'cold-scratch-off-512']:
        clock = stages[name]['actual_clock']
        checks[name + '_actual_ticks'] = clock['end_tick'] - clock['start_tick'] == 512
        checks[name + '_no_error_resume'] = clock['error_resumes'] == 0
        checks[name + '_payments_valid'] = stages[name]['integrity']['payments_valid']
    result = {'source_revision': subprocess.check_output(['git', '-C', ROOT, 'rev-parse', 'HEAD'], text=True).strip(),
              'checkpoint_sha256': runner.sha256(a.checkpoint), 'stages': stages, 'checks': checks}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    if not all(checks.values()):
        raise ValueError(checks)
    print(json.dumps(checks, indent=2), flush=True)
finally:
    os.close(lock)
    lock_path.unlink()
