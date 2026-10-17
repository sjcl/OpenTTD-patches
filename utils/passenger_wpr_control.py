#!/usr/bin/env python3
"""Separate code effects from machine-speed drift between two WPR exports of one mode.

Both exports must replay the same deterministic ticks (equal canonical end states),
so code that neither change touched did identical work. Its sampled CPU ratio
new/old estimates the machine factor (clock, thermal and cache state), and the
total of the new run divided by that factor is the drift-corrected comparison.

    passenger_wpr_control.py OLD_EVIDENCE NEW_EVIDENCE [--mode legacy hyperpath]
        [--touched WORD ...] [--ticks 2048]

Each evidence directory holds MODE/CPU_Usage_(Sampled)_Stacks.csv.gz. Only main-thread
samples with StateGameLoop on the stack are used. A sample is "heap" when any frame is
an ntdll heap routine, "touched" when a frame contains one of the touched words
(default: YAPF, passenger and cargo-list code), otherwise "untouched".
"""
import argparse
import csv
import gzip
import json
from pathlib import Path
import re

SPLIT = re.compile(r'/(?=(?:[A-Za-z0-9_.\-]+\.(?:exe|dll|sys|EXE|DLL|SYS)!|\[))')
TOUCHED = ('Yapf', 'NodeList', 'BumpAlloc', 'Passenger', 'StationCargoList', 'LoadUnload', 'CargoList')


def is_heap(frame):
    return frame.startswith('ntdll.dll!') and any(k in frame for k in ('Heap', 'Block', 'LowFrag', 'Rtlp'))


def categories(path, touched):
    totals = {'untouched': 0.0, 'touched': 0.0, 'heap': 0.0}
    with gzip.open(path, 'rt', encoding='utf-8', errors='replace') as stream:
        rows = csv.reader(stream)
        next(rows)
        for row in rows:
            if len(row) < 5 or not row[1] or not row[2]:
                continue  # Process and thread subtotal rows.
            frames = SPLIT.split(row[2])
            if 'openttd.exe!StateGameLoop' not in frames:
                continue
            weight = float(row[3].replace(',', ''))
            if any(is_heap(f) for f in frames):
                totals['heap'] += weight
            elif any(word in f for f in frames for word in touched):
                totals['touched'] += weight
            else:
                totals['untouched'] += weight
    return totals


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('old', type=Path)
    parser.add_argument('new', type=Path)
    parser.add_argument('--mode', nargs='+', default=['legacy', 'hyperpath'])
    parser.add_argument('--touched', nargs='+', default=list(TOUCHED))
    parser.add_argument('--ticks', type=int, default=2048)
    args = parser.parse_args()
    result = {}
    for mode in args.mode:
        stacks = 'CPU_Usage_(Sampled)_Stacks.csv.gz'
        old = {k: v / args.ticks for k, v in categories(args.old / mode / stacks, args.touched).items()}
        new = {k: v / args.ticks for k, v in categories(args.new / mode / stacks, args.touched).items()}
        factor = new['untouched'] / old['untouched']
        old_total, new_total = sum(old.values()), sum(new.values())
        result[mode] = dict(old_ms_per_tick=old, new_ms_per_tick=new, machine_factor=factor,
            raw_delta_ms_per_tick=new_total - old_total, corrected_delta_ms_per_tick=new_total / factor - old_total,
            corrected_touched_delta=new['touched'] / factor - old['touched'], corrected_heap_delta=new['heap'] / factor - old['heap'])
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
