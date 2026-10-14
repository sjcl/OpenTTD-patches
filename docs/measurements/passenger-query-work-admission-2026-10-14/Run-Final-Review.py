"""Balanced A/B/C/C/B/A runs on the fixed map; retry only contaminated runs."""
import argparse
import importlib.util
import json
import os
import pathlib
import re
import tempfile
from types import SimpleNamespace

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('runner', HERE.parent / 'passenger-routing-profile-2026-10-13/Run-Profile.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('build', type=pathlib.Path)
    p.add_argument('package', type=pathlib.Path)
    p.add_argument('--output', default='final-review-comparison')
    a = p.parse_args()
    build, package = a.build.resolve(), a.package.resolve()
    if runner.sha256(package / 'input.sav') != '14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390':
        raise ValueError('Wrong fixed input')
    output = build / a.output
    output.mkdir(exist_ok=False)
    lock_path = pathlib.Path(tempfile.gettempdir()) / 'openttd-passenger-routing-measurement.lock'
    lock = runner.acquire_measurement_lock(lock_path, 14400)
    manifests, excluded = [], []
    try:
        os.write(lock, json.dumps({'pid': os.getpid(), 'task': 'Final corrected admission A/B/C comparison'}).encode())
        sequence = [('control', 'on'), ('q16-t49152', 'on'), ('q16-t49152', 'cold'),
                    ('q16-t49152', 'cold'), ('q16-t49152', 'on'), ('control', 'on')]
        for index, (variant, mode) in enumerate(sequence, 1):
            binary = build / f'openttd-{variant}.exe'
            metadata = json.loads(binary.with_suffix('.instrumentation.json').read_text(encoding='utf-8'))
            for attempt in range(1, 4):
                target = output / f'{index:02}-{variant}-{mode}-attempt{attempt}'
                args = SimpleNamespace(package=package, binary=binary, build=build, output=target,
                    label=variant, revision=metadata['source_revision'], kind='minimal', ticks=8192,
                    phase='initial_8192', repetitions=1, modes=[mode], cache_off_scope='all', resume_settings=False,
                    wait_idle_seconds=14400, idle_grace_seconds=90)
                try:
                    runner.run_locked(args)
                except RuntimeError:
                    path = target / 'manifest.json'
                    failed = json.loads(path.read_text(encoding='utf-8')) if path.exists() else {}
                    if not any(run.get('contamination') for run in failed.get('runs', [])):
                        raise
                    excluded.append(str(path))
                    runner.dump(output / 'excluded-runs.json', {'manifests': excluded, 'reason': 'host contention'})
                    continue
                manifest = json.loads((target / 'manifest.json').read_text(encoding='utf-8'))
                run = manifest['runs'][0]
                console = (target / run['directory'] / (run['id'] + '-console.txt')).read_text(encoding='utf-8-sig')
                ticks = [int(t) for t in re.findall(r'^State ticks: (-?\d+)', console, re.M)]
                if ticks != [3176545487, 3176553679]:
                    raise ValueError(f'Wrong actual interval: {ticks}')
                run['final_save_bytes'] = (target / run['directory'] / (run['id'] + '-result.sav')).stat().st_size
                manifest['balanced_sequence_index'] = index
                runner.dump(target / 'manifest.json', manifest)
                manifests.append(str(target / 'manifest.json'))
                runner.dump(output / 'sequence.json', {'manifests': manifests, 'excluded': excluded, 'status': 'running'})
                break
            else:
                raise RuntimeError('Repeatedly contaminated condition; no uncontaminated measurement')
        runner.dump(output / 'sequence.json', {'manifests': manifests, 'excluded': excluded, 'status': 'complete'})
    finally:
        os.close(lock)
        lock_path.unlink()


if __name__ == '__main__':
    main()
