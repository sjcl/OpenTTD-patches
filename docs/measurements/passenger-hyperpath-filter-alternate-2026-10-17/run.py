"""Run one alternate-mode real-map measurement with the previous fixed tick window."""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(__doc__)
    for name in ('binary', 'package', 'lang', 'output'):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, '-B', str(root / 'utils/passenger_precomputed_real_map.py'), 'run',
               str(args.binary.resolve()), str(args.package.resolve()), str(args.output.resolve()),
               '--lang', str(args.lang.resolve()), '--ticks', '2048', '--timeout', '1800',
               '--measurement-wait', '60', '--precomputed', '--hyperpath', '--cold-precomputed',
               '--warm-precomputed', '--wait-background-ready', '--mid-rebuild-tick', '1024',
               '--finish-rebuild-precomputed', '--main-thread-affinity-mask', '1',
               '--hyperpath-filter-alternate']
    env = {k: v for k, v in os.environ.items() if not k.startswith('OPENTTD_')}
    with args.output.with_name(args.output.name + '-runner.log').open('wb') as stream:
        subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)


if __name__ == '__main__':
    main()
