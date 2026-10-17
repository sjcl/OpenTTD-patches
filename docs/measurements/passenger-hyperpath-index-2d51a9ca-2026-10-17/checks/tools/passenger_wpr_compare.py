"""Compare like-mode canonical state and YAPF-scoped heap samples between two WPR runs."""
import argparse
from collections import defaultdict
import csv
from decimal import Decimal
import gzip
import json
from pathlib import Path
import re


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def yapf(directory):
    metadata = read_json(directory / 'process.json')
    tid = str(metadata['main_thread']['thread_id'])
    clock = metadata['actual-ticks.json']
    ticks = clock['end_tick'] - clock['start_tick']
    assert ticks == len(clock['samples']) == 2048
    functions = defaultdict(lambda: [0, 0, 0, 0])
    total_ns = total_samples = heap_ns = heap_samples = 0
    with gzip.open(directory / 'CPU_Usage_(Sampled)_Stacks.csv.gz', 'rt', encoding='utf-8-sig', newline='') as stream:
        for row in csv.DictReader(stream):
            if row['Thread ID'] != tid or not row['Stack']:
                continue
            frames = re.split(r'/(?=[^/!]+\.(?:exe|dll|sys)!)', row['Stack'], flags=re.I)
            # Include pathfinder entry points, template bodies and their container destructors.
            if not any(f.startswith('openttd.exe!') and 'yapf' in f.lower() for f in frames):
                continue
            ns = int(Decimal(row['Weight (ms)'].replace(',', '')) * 1000000)
            count = int(row['Count'].replace(',', ''))
            total_ns += ns
            total_samples += count
            if 'ntdll.dll!RtlAllocateHeap' in frames or 'ntdll.dll!RtlFreeHeap' in frames:
                heap_ns += ns
                heap_samples += count
            for name in set(frames):
                if name != '[Root]':
                    functions[name][0] += ns
                    functions[name][1] += count
            functions[frames[-1]][2] += ns
            functions[frames[-1]][3] += count
    assert sum(v[2] for v in functions.values()) == total_ns
    assert sum(v[3] for v in functions.values()) == total_samples
    assert total_samples > 0
    return functions, dict(ticks=ticks, yapf_ns=total_ns, yapf_samples=total_samples,
                          yapf_cpu_ms_per_tick=total_ns/1000000/ticks, heap_union_ns=heap_ns,
                          heap_union_samples=heap_samples, heap_union_ms_per_tick=heap_ns/1000000/ticks)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    parser.add_argument('--allow-hyperpath-filter-change', action='store_true',
                        help='Compare identical ticks across mask/index strategies; canonical equality is still required')
    args = parser.parse_args()
    summary = dict(reference=str(args.before), yapf_scope='Main-thread sample with Yapf in any openttd.exe frame; inclusive counts a function once per stack; exclusive is its stack leaf.', modes={})
    rows, canonical_rows, branch_rows = [], [], []
    oldsummary, newsummary = read_json(args.before/'summary.json'), read_json(args.after/'summary.json')
    for mode in ['legacy', 'hyperpath']:
        before, after = args.before / mode, args.after / mode
        oldmeta, newmeta = read_json(before/'process.json'), read_json(after/'process.json')
        for key in ['input_sha256', 'language_sha256', 'precomputed', 'hyperpath',
                    'hyperpath_filter_alternate', 'cold_precomputed', 'warm_precomputed', 'mid_rebuild_tick', 'main_thread_affinity_mask']:
            assert oldmeta[key] == newmeta[key], (mode, key)
        def filter_mode(meta):
            explicit = meta.get('hyperpath_filter_mode')
            if explicit is not None:
                assert meta['hyperpath_filter'] is None and not meta['hyperpath_filter_alternate']
                return explicit
            return 'alternate' if meta['hyperpath_filter_alternate'] else {True: 'on', False: 'off', None: 'default'}[meta['hyperpath_filter']]
        oldfilter, newfilter = filter_mode(oldmeta), filter_mode(newmeta)
        if args.allow_hyperpath_filter_change:
            assert oldfilter in ['on', 'index'] and newfilter in ['on', 'index'], (mode, oldfilter, newfilter)
        else:
            assert oldfilter == newfilter, (mode, oldfilter, newfilter)
        assert newmeta['exit_code'] == 0 and newmeta['simulation_timing_valid'] and newmeta['warm_complete']
        oldstate, newstate = read_json(before/'save-integrity.json'), read_json(after/'save-integrity.json')
        mismatches = []
        for key in sorted(oldstate.keys() | newstate.keys()):
            equal = key in oldstate and key in newstate and oldstate[key] == newstate[key]
            row = dict(mode=mode, field=key, before=oldstate.get(key), after=newstate.get(key), equal=equal)
            canonical_rows.append(row)
            if not equal:
                mismatches.append(row)
        old, old_summary = yapf(before)
        new, new_summary = yapf(after)
        targets = {}
        for function in old.keys() | new.keys():
            left, right = old[function], new[function]
            row = dict(mode=mode, function=function, before_inclusive_ms_per_tick=f'{left[0]/1000000/2048:.9f}',
                       after_inclusive_ms_per_tick=f'{right[0]/1000000/2048:.9f}',
                       delta_inclusive_ms_per_tick=f'{(right[0]-left[0])/1000000/2048:.9f}',
                       before_exclusive_ms_per_tick=f'{left[2]/1000000/2048:.9f}',
                       after_exclusive_ms_per_tick=f'{right[2]/1000000/2048:.9f}',
                       delta_exclusive_ms_per_tick=f'{(right[2]-left[2])/1000000/2048:.9f}',
                       inclusive_reduction_percent=f'{100*(left[0]-right[0])/left[0]:.6f}' if left[0] else '',
                       exclusive_reduction_percent=f'{100*(left[2]-right[2])/left[2]:.6f}' if left[2] else '',
                       before_inclusive_samples=left[1], after_inclusive_samples=right[1],
                       before_exclusive_samples=left[3], after_exclusive_samples=right[3])
            rows.append(row)
            if function in ['ntdll.dll!RtlpFindEntry', 'ntdll.dll!RtlpInsertFreeBlock']:
                targets[function] = row
        for function in ['ntdll.dll!RtlpFindEntry', 'ntdll.dll!RtlpInsertFreeBlock']:
            assert function in targets and int(targets[function]['before_inclusive_samples']) > 0
        summary['modes'][mode] = dict(filter_before=oldfilter, filter_after=newfilter,
                                     canonical_fields=len(oldstate), canonical_equal=oldstate == newstate,
                                     mismatches=mismatches, before=old_summary, after=new_summary, targets=targets,
                                     targets_reduced=all(Decimal(row['after_inclusive_ms_per_tick']) < Decimal(row['before_inclusive_ms_per_tick']) for row in targets.values()))
        summary['modes'][mode]['whole_tick_before_after'] = {
            'before_simulation_wall_ms_per_tick': oldsummary[mode]['simulation_wall_ms_per_tick'],
            'after_simulation_wall_ms_per_tick': newsummary[mode]['simulation_wall_ms_per_tick'],
            'before_sampled_cpu_ms_per_tick': oldsummary[mode]['sampled_cpu_ms_per_tick'],
            'after_sampled_cpu_ms_per_tick': newsummary[mode]['sampled_cpu_ms_per_tick']}
        oldbranches = {row['branch']: Decimal(row[mode+'_ms_per_tick']) for row in oldsummary['branches']}
        newbranches = {row['branch']: Decimal(row[mode+'_ms_per_tick']) for row in newsummary['branches']}
        for branch in oldbranches.keys() | newbranches.keys():
            left, right = oldbranches.get(branch, Decimal(0)), newbranches.get(branch, Decimal(0))
            branch_rows.append(dict(mode=mode, branch=branch, before_ms_per_tick=str(left),
                                    after_ms_per_tick=str(right), delta_ms_per_tick=str(right-left)))
    for filename, data in [('yapf-before-after.csv', rows), ('canonical-comparison.csv', canonical_rows),
                           ('tick-branches-before-after.csv', branch_rows)]:
        data.sort(key=lambda row: (row['mode'], row.get('function', row.get('field', row.get('branch')))))
        with (args.after / filename).open('w', encoding='utf-8', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(data[0]), lineterminator='\n')
            writer.writeheader()
            writer.writerows(data)
    (args.after / 'comparison.json').write_text(json.dumps(summary, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    if not all(mode['canonical_equal'] for mode in summary['modes'].values()):
        raise SystemExit('Canonical mismatch: comparison artifacts retain every difference')


if __name__ == '__main__':
    main()
