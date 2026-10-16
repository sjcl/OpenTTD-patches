#!/usr/bin/env python3
"""Summarize matched B-mode transport with independent ordinary/focused samples."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

import passenger_precomputed_real_map as real


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('runs', type=Path, help='Directory containing previous/ and hyperpath/')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    runs = [args.runs / name for name in ('previous', 'hyperpath')]
    for run in runs:
        process = json.loads((run / 'process.json').read_text(encoding='utf-8'))
        ticks = process['actual-ticks.json']
        if process['exit_code'] or process['contamination'] or ticks['end_tick'] - ticks['start_tick'] != process['requested_ticks'] or len(ticks['samples']) != process['requested_ticks']:
            raise ValueError(f'Incomplete or contaminated measurement: {run}')
    real.summarize(argparse.Namespace(output=args.output / 'simulation.json', runs=runs))
    root = Path(__file__).resolve().parent
    def summarize(output, inputs):
        subprocess.run([sys.executable, '-B', str(root / 'passenger_transport_summary.py'), str(output), *map(str, inputs)], check=True)
    summarize(args.output / 'transport-ordinary.json', runs)
    focused = []
    for run in runs:
        view = run / 'focus-view'
        view.mkdir(exist_ok=True)
        for source in run.glob('focused-transport-*'):
            shutil.copy2(source, view / source.name.removeprefix('focused-'))
        process = json.loads((run / 'process.json').read_text(encoding='utf-8'))
        process.pop('actual-ticks.json', None)
        process['focused_od_trace'] = True
        (view / 'process.json').write_text(json.dumps(process) + '\n', encoding='utf-8')
        focused.append(view)
    summarize(args.output / 'transport-focused.json', focused)


if __name__ == '__main__':
    main()
