"""Extract like-mode membership CPU times and apply the supplied control factors."""
import argparse
import csv
import json
from pathlib import Path


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    args = parser.parse_args()
    control = read_json(args.after / 'control.json')
    comparison = read_json(args.after / 'comparison.json')
    summaries = [read_json(path / 'summary.json') for path in [args.before, args.after]]
    function = 'openttd.exe!PassengerJourneyLoadingContext::MayUseHyperpathPolicy'
    output = []
    for mode in ['legacy', 'hyperpath']:
        assert comparison['modes'][mode]['canonical_equal']
        factor = control[mode]['machine_factor']
        for index, age in enumerate(['old', 'new']):
            total = sum(control[mode][age + '_ms_per_tick'].values())
            assert abs(total - summaries[index][mode]['sampled_cpu_ms_per_tick']['simulation_stack']) < 1e-8
        tables = []
        for directory in [args.before, args.after]:
            with (directory / (mode + '-functions.csv')).open(encoding='utf-8', newline='') as stream:
                tables.append({(row['scope'], row['function']): row for row in csv.DictReader(stream)})
        for scope in ['all', 'main', 'simulation_stack']:
            left, right = [table.get((scope, function), {}) for table in tables]
            row = dict(mode=mode, scope=scope, function=function, machine_factor=factor)
            for kind in ['inclusive', 'exclusive']:
                old = float(left.get(kind + '_ms_per_tick', '0'))
                new = float(right.get(kind + '_ms_per_tick', '0'))
                corrected = new / factor
                row.update({
                    'before_' + kind + '_ms_per_tick': old,
                    'after_' + kind + '_ms_per_tick': new,
                    'raw_' + kind + '_delta_ms_per_tick': new - old,
                    'corrected_after_' + kind + '_ms_per_tick': corrected,
                    'corrected_' + kind + '_delta_ms_per_tick': corrected - old,
                    'raw_' + kind + '_reduction_percent': 100 * (1 - new / old) if old else '',
                    'corrected_' + kind + '_reduction_percent': 100 * (1 - corrected / old) if old else '',
                    'before_' + kind + '_samples': int(left.get(kind + '_samples', '0')),
                    'after_' + kind + '_samples': int(right.get(kind + '_samples', '0')),
                })
            output.append(row)
    with (args.after / 'membership-before-after.csv').open('w', encoding='utf-8', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(output[0]), lineterminator='\n')
        writer.writeheader()
        writer.writerows(output)
    print(json.dumps([row for row in output if row['scope'] == 'simulation_stack'], indent=2))


if __name__ == '__main__':
    main()
