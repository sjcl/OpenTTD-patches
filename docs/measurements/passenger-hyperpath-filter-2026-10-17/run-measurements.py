"""Replay one fixed real map OFF/ON/ON/OFF, with an exact midpoint replacement."""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(__doc__)
    for name in ('binary', 'package', 'lang', 'output'):
        parser.add_argument(name, type=Path)
    parser.add_argument('--ticks', type=int, default=2048)
    args = parser.parse_args()
    if args.ticks < 2 or args.ticks % 2:
        parser.error('--ticks must be a positive even number >= 2')
    root = Path(__file__).resolve().parents[3]
    runner = root / 'utils/passenger_precomputed_real_map.py'
    args.output.mkdir(parents=True, exist_ok=False)
    # Each child starts with the same product settings and no inherited probe controls.
    env = {k: v for k, v in os.environ.items() if not k.startswith('OPENTTD_')}
    for label, enabled in [('off-a', False), ('on-a', True), ('on-b', True), ('off-b', False)]:
        command = [sys.executable, '-B', str(runner), 'run', str(args.binary.resolve()),
                   str(args.package.resolve()), str((args.output / label).resolve()),
                   '--lang', str(args.lang.resolve()), '--ticks', str(args.ticks),
                   '--timeout', '1800', '--measurement-wait', '60', '--precomputed', '--hyperpath',
                   '--cold-precomputed', '--warm-precomputed', '--wait-background-ready',
                   '--mid-rebuild-tick', str(args.ticks // 2), '--finish-rebuild-precomputed',
                   '--main-thread-affinity-mask', '1',
                   '--hyperpath-filter' if enabled else '--no-hyperpath-filter']
        print(f'Starting {label}', flush=True)
        with (args.output / f'{label}-runner.log').open('wb') as log:
            subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        print(f'Completed {label}', flush=True)


if __name__ == '__main__':
    main()
