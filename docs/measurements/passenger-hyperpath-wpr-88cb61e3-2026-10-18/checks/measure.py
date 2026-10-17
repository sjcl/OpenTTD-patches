"""Replay the original per-mode 8462e6cd steady inputs; never replace a missing save.

Run --check-inputs-only first, then start passenger_wpr_record.ps1 elevated and
run this script unelevated. Export with passenger_wpr_export.py afterwards.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'utils/passenger_precomputed_real_map.py').is_file())
REFERENCE = ROOT / 'docs/measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18'
EXPECTED = {
    'hyperpath': '275544f1fd7bb16920ebd325c3a916ee06f164fcbe53abf0c1bb6409e9802f9f',
    'legacy': 'b276c5de400651f646c129ab394bc97a686f38aa345b3a4a799e07c8a06289ca',
}


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--hyperpath-package', type=Path)
    parser.add_argument('--legacy-package', type=Path)
    parser.add_argument('--mode', nargs='+', choices=['legacy', 'hyperpath'], default=['legacy', 'hyperpath'])
    parser.add_argument('--check-inputs-only', action='store_true')
    args = parser.parse_args()
    build = args.build.resolve()
    packages = {}
    for mode in args.mode:
        package = getattr(args, mode + '_package')
        if package is None:
            parser.error('--' + mode + '-package is required for this mode')
        package = package.resolve()
        # Check the actual bytes, independently of any supplied manifest.
        if sha(package / 'input.sav') != EXPECTED[mode]:
            raise ValueError(mode + ': input differs from the original frozen 8192-tick seed')
        reference = (REFERENCE / 'frozen-legacy/package.json' if mode == 'legacy' else
            ROOT / 'docs/measurements/passenger-hyperpath-steady-8462e6cd-2026-10-17/frozen/package.json')
        manifest = load(reference)
        assert sha(package / 'openttd.cfg') == manifest['config_sha256']
        for asset in manifest['assets']:
            assert sha(package / asset['path']) == asset['sha256'], asset['path']
        packages[mode] = package
        print(mode + ': frozen save, config and all assets match the baseline', flush=True)
    if args.check_inputs_only:
        return
    assert (build / 'Release/openttd.exe').is_file() and (build / 'Release/openttd.pdb').is_file()
    env = dict(os.environ)
    for key in ['OPENTTD_PASSENGER_TRACE', 'OPENTTD_PASSENGER_CHECKPOINT_TICK',
                'OPENTTD_PASSENGER_FOCUS_TRACE', 'OPENTTD_PASSENGER_DEPARTURE_DIAGNOSTIC',
                'OPENTTD_RETIREMENT_PROFILE', 'OPENTTD_RETIREMENT_REFERENCE']:
        env.pop(key, None)
    comparisons = {}
    for mode in args.mode:
        output = build / mode
        command = [sys.executable, '-B', str(ROOT / 'utils/passenger_precomputed_real_map.py'),
            'run', str(build / 'Release/openttd.exe'), str(packages[mode]), str(output),
            '--ticks', '2048', '--timeout', '2400', '--measurement-wait', '2400',
            '--resume', '--precomputed', '--hyperpath' if mode == 'hyperpath' else '--no-hyperpath',
            '--warm-precomputed', '--wait-background-ready', '--hyperpath-filter-mode', 'index',
            '--main-thread-affinity-mask', '1', '--lang', str(build / 'lang'), '--wpr-profile']
        with (build / (mode + '-runner.log')).open('wb') as stream:
            subprocess.run(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
        metadata = load(output / 'process.json')
        clock = metadata['actual-ticks.json']
        assert metadata['exit_code'] == 0 and metadata['simulation_timing_valid'] is True
        assert metadata['input_sha256'] == EXPECTED[mode]
        assert clock['start_tick'] == 3181127773 and clock['end_tick'] == 3181129821
        assert len(clock['samples']) == 2048
        previous = load(REFERENCE / mode / 'save-integrity.json')
        current = load(output / 'save-integrity.json')
        assert len(previous) == len(current) == 24
        diff = {k: dict(previous=previous.get(k), current=current.get(k)) for k in previous.keys() | current.keys()
                if previous.get(k) != current.get(k)}
        comparisons[mode] = dict(equal=not diff, fields=24, differences=diff, input_sha256=EXPECTED[mode])
        (build / 'canonical-comparison.json').write_text(json.dumps(comparisons, indent=2) + '\n', encoding='utf-8')
        assert not diff, mode + ': canonical mismatch: ' + str(diff)
        print(mode + ': canonical 24/24 identical', flush=True)


if __name__ == '__main__':
    main()
