"""Recheck the real retired-v4 fixture with the merged native field-skip binary."""
import argparse
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
spec = importlib.util.spec_from_file_location('runner', HERE.parent / 'passenger-routing-profile-2026-10-13/Run-Profile.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
spec = importlib.util.spec_from_file_location('feature', HERE / 'Read-Save-Feature.py')
feature = importlib.util.module_from_spec(spec)
spec.loader.exec_module(feature)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('review', type=pathlib.Path)
p.add_argument('package', type=pathlib.Path)
p.add_argument('input', type=pathlib.Path)
a = p.parse_args()
out = a.review.resolve()
result_path = out / 'result.json'
result = json.loads(result_path.read_text(encoding='utf-8'))
assert all(result['checks'].values())
assert runner.sha256(a.input) == '2fa1b07407db3ca3077664fb9aab21a13378d2df6d478c80d4cf81302495f1c2'
assert feature.routing_feature(a.input) == 4
reference = json.loads((HERE / 'final-save-review/result.json').read_text(encoding='utf-8'))['stages']['v4-review-scanned-reader']['integrity']
lock_path = pathlib.Path(tempfile.gettempdir()) / 'openttd-passenger-routing-measurement.lock'
lock = runner.acquire_measurement_lock(lock_path, 14400)
try:
    os.write(lock, json.dumps({'pid': os.getpid(), 'task': 'Merged native v4 compatibility'}).encode())
    for name, input_path, ticks in [('v4-load', a.input.resolve(), 0),
                                   ('v4-reload-v3', out / 'v4-load/result.sav', 0),
                                   ('v4-continue-256', out / 'v4-load/result.sav', 256)]:
        runner.wait_for_idle(14400, grace=20)
        with (out / (name + '.log')).open('w', encoding='utf-8') as log:
            subprocess.run([sys.executable, HERE / 'Run-Save-Stage.py', '--binary', out / 'openttd-save-inspector.exe',
                            '--package', a.package.resolve(), '--input', input_path, '--output', out / name, '--ticks', str(ticks)],
                           cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        result['stages'][name] = json.loads((out / name / 'stage.json').read_text(encoding='utf-8'))
    keys = ['packets', 'cargo_units', 'known_od_units', 'journey_packets', 'packet_state_digest', 'payment_digest', 'payments_valid']
    for name in ['v4-load', 'v4-reload-v3']:
        result['checks'][name + '_saved_fields_conserved'] = all(reference[k] == result['stages'][name]['integrity'][k] for k in keys)
    formats = {name: feature.routing_feature(out / name / 'result.sav') for name in result['stages']}
    result['checks']['all_saved_formats_v3'] = all(v == 3 for v in formats.values())
    continued = result['stages']['v4-continue-256']
    result['checks']['v4_actual_256_ticks'] = continued['actual_clock']['end_tick'] - continued['actual_clock']['start_tick'] == 256
    result['checks']['v4_no_error_resume'] = continued['actual_clock']['error_resumes'] == 0
    result['checks']['v4_continued_payments_valid'] = continued['integrity']['payments_valid']
    result['v4_reference'] = {'input_sha256': runner.sha256(a.input), 'actual_input_format': 4,
                              'retired_true_packets': reference['legacy_true_packets'], 'retired_false_packets': reference['legacy_false_packets'],
                              'saved_formats': formats, 'reference': 'final-save-review/result.json'}
    result_path.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    if not all(result['checks'].values()):
        raise ValueError(result['checks'])
    print(json.dumps(result['checks'], indent=2), flush=True)
finally:
    os.close(lock)
    lock_path.unlink()
