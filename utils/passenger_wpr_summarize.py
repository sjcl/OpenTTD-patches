"""Aggregate the saved WPA sampled-CPU CSVs; inclusive counts each function once per stack."""
import argparse
from collections import defaultdict
import csv
from decimal import Decimal
import gzip
import json
from pathlib import Path
import re
import shutil
import statistics

TICKS = 2048
UNIT = 1000000  # Exported milliseconds -> integer nanoseconds.


def number(text):
    return int(Decimal(text.replace(',', '')) * UNIT)


def table(path):
    with path.open(encoding='utf-8-sig', newline='') as stream:
        yield from csv.DictReader(stream)


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def write_csv(path, fields, rows):
    with path.open('w', encoding='utf-8', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)


def compress(source, target):
    with source.open('rb') as src, target.open('wb') as dst:
        with gzip.GzipFile(filename='', fileobj=dst, mode='wb', mtime=0) as out:
            shutil.copyfileobj(src, out)


def aggregate(build, evidence, mode, export_name):
    directory = build / mode
    output = evidence / mode
    output.mkdir(exist_ok=True)
    metadata = json.loads((directory / 'process.json').read_text(encoding='utf-8'))
    clock = metadata['actual-ticks.json']
    assert len(clock['samples']) == clock['end_tick'] - clock['start_tick'] == TICKS
    pid, tid = map(int, (directory / 'wpr-ready.txt').read_text().split())
    assert tid == metadata['main_thread']['thread_id']
    process = f'openttd.exe ({pid})'
    stats = {s: defaultdict(lambda: [0, 0, 0, 0]) for s in ['all', 'main', 'simulation_stack']}
    totals = defaultdict(lambda: [0, 0])
    branches = defaultdict(lambda: [0, 0])
    missing = defaultdict(lambda: [0, 0])
    callers, allocations = defaultdict(int), defaultdict(int)
    product_samples, unresolved_product = 0, defaultdict(int)
    roots = ['openttd.exe!RunPassengerWaitingEvents', 'openttd.exe!LoadUnloadVehicle',
             'openttd.exe!PassengerJourneyLoadingContext::MayUseHyperpathPolicy',
             'openttd.exe!PassengerServiceDatabase::PreparePrecomputed']
    # Slot order: inclusive nanoseconds, inclusive samples, exclusive nanoseconds, exclusive samples.
    for row in table(directory / export_name / 'CPU_Usage_(Sampled)_Stacks.csv'):
        assert row['Process'] == process
        if not row['Thread ID'] or not row['Stack']:
            continue  # Process/thread subtotal rows are not samples.
        weight, count = number(row['Weight (ms)']), int(row['Count'].replace(',', ''))
        main = int(row['Thread ID']) == tid
        totals['all'][0] += weight
        totals['all'][1] += count
        if main:
            totals['main'][0] += weight
            totals['main'][1] += count
        stack = row['Stack']
        frames = re.split(r'/(?=[^/!]+\.(?:exe|dll|sys)!)', stack, flags=re.I)
        frames = [f for f in frames if f != '[Root]']
        if not frames or frames == ['n/a']:
            frames = ['[unavailable stack]']
            missing['all'][0] += weight
            missing['all'][1] += count
            if main:
                missing['main'][0] += weight
                missing['main'][1] += count
        simulation = main and 'openttd.exe!StateGameLoop' in frames
        if main:
            for root in roots:
                if root in frames:
                    index = frames.index(root)
                    child = frames[index + 1] if index + 1 < len(frames) else '[self]'
                    callers[(root, child)] += weight
        if simulation:
            heaps = [(i, f) for i, f in enumerate(frames) if f in ['ntdll.dll!RtlAllocateHeap', 'ntdll.dll!RtlFreeHeap']]
            if heaps:
                index, heap = heaps[0]
                candidates = [f for f in frames[:index] if f.startswith('openttd.exe!') and not f.startswith(
                    ('openttd.exe!std::', 'openttd.exe!_', 'openttd.exe!operator ', 'openttd.exe!free', 'openttd.exe!malloc', 'openttd.exe!common_'))]
                caller = candidates[-1] if candidates else '[no named application caller]'
                allocations[(heap, caller)] += weight
        scopes = ['all'] + (['main'] if main else []) + (['simulation_stack'] if simulation else [])
        for scope in scopes:
            for function in set(frames):
                stats[scope][function][0] += weight
                stats[scope][function][1] += count
        if simulation:
            stats['simulation_stack'][frames[-1]][2] += weight
            stats['simulation_stack'][frames[-1]][3] += count
            totals['simulation_stack'][0] += weight
            totals['simulation_stack'][1] += count
            index = frames.index('openttd.exe!StateGameLoop')
            branch = frames[index + 1] if index + 1 < len(frames) else '[StateGameLoop self]'
        elif main:
            branch = '[outside StateGameLoop]'
        else:
            branch = '[worker threads]'
        branches[branch][0] += weight
        branches[branch][1] += count
    # Use WPA's sampled instruction-pointer Function column for whole-thread exclusive attribution.
    for row in table(directory / export_name / 'CPU_Usage_(Sampled)_Exclusive_functions.csv'):
        assert row['Process'] == process
        if not row['Thread ID'] or not row['Function']:
            continue
        weight, count = number(row['Weight (ms)']), int(row['Count'].replace(',', ''))
        function = row['Module'] + '!' + row['Function']
        if row['Module'] == 'openttd.exe':
            product_samples += count
            if any(marker in row['Function'].lower() for marker in ['not found', 'unknown', 'not loaded', 'n/a']):
                unresolved_product[row['Function']] += count
        for scope in ['all'] + (['main'] if int(row['Thread ID']) == tid else []):
            stats[scope][function][2] += weight
            stats[scope][function][3] += count
    checks = {}
    for scope in ['all', 'main', 'simulation_stack']:
        actual = [sum(v[i] for v in stats[scope].values()) for i in [2, 3]]
        checks[scope] = dict(stack_total_ns=totals[scope][0], stack_samples=totals[scope][1],
                             exclusive_total_ns=actual[0], exclusive_samples=actual[1])
        assert actual == totals[scope], (mode, scope, checks[scope])
    rows = []
    for scope, functions in stats.items():
        for function, values in sorted(functions.items(), key=lambda pair: (-pair[1][2], pair[0])):
            rows.append(dict(scope=scope, function=function, inclusive_ms=f'{values[0]/UNIT:.6f}', inclusive_samples=values[1],
                             exclusive_ms=f'{values[2]/UNIT:.6f}', exclusive_samples=values[3],
                             inclusive_ms_per_tick=f'{values[0]/UNIT/TICKS:.9f}', exclusive_ms_per_tick=f'{values[2]/UNIT/TICKS:.9f}'))
    write_csv(evidence / (mode + '-functions.csv'), list(rows[0]), rows)
    for path in (directory / export_name).glob('*.csv'):
        compress(path, output / (path.name + '.gz'))
    for name in ['process.json', 'actual-ticks.json', 'save-integrity.json', 'trace-stats.txt', 'export-command.json',
                 'wpr-ready.txt', 'wpr-go.txt', 'wpr-finished.txt', 'wpr-saved.txt', 'wpa-export.log']:
        shutil.copy2(directory / name, output / name)
    for name in ['on-1-console.txt', 'stdout.txt', 'stderr.txt']:
        compress(directory / name, output / (name + '.gz'))
    shutil.copytree(directory / 'scripts', output / 'scripts', dirs_exist_ok=True)
    export = json.loads((directory / 'export-command.json').read_text(encoding='utf-8'))
    trace_stats = (directory / 'trace-stats.txt').read_text(encoding='utf-8-sig')
    assert 'Total # Lost Buffers : 0' in trace_stats and 'Total # Lost Events  : 0' in trace_stats
    log = (directory / 'wpa-export.log').read_text(encoding='utf-8', errors='replace')
    summary = dict(pid=pid, main_thread_id=tid, start_tick=clock['start_tick'], end_tick=clock['end_tick'], ticks=TICKS,
                   etl_sha256=export['trace_sha256'], etl_bytes=export['trace_bytes'], lost_events=0, lost_buffers=0,
                   product_function_samples=product_samples, unresolved_product_functions=dict(unresolved_product),
                   wpa_invalid_event_warning=re.findall(r'Warning: EventSink[\s\S]+?MofLength: \d+', log),
                   driver_loops=clock['driver_loops'],
                   simulation_wall_ms_per_tick=statistics.mean(s[1] for s in clock['samples'])/1000,
                   journey_wall_ms_per_tick=statistics.mean(s[2] for s in clock['samples'])/1000,
                   sampled_cpu_ms_per_tick={s: v[0]/UNIT/TICKS for s, v in totals.items()},
                   unavailable_stack={s: dict(ns=v[0], samples=v[1]) for s, v in missing.items()},
                   conservation_checks=checks, warnings=metadata['newgrf_warning_lines'],
                   payments_valid=metadata['save-integrity.json']['payments_valid'])
    return stats, branches, summary, callers, allocations


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('evidence', type=Path)
    parser.add_argument('--export-name', default='wpa-v3')
    args = parser.parse_args()
    legacy, lbranches, lsummary, lcallers, lalloc = aggregate(args.build, args.evidence, 'legacy', args.export_name)
    hyperpath, hbranches, hsummary, hcallers, halloc = aggregate(args.build, args.evidence, 'hyperpath', args.export_name)
    for name, left, right, fields in [('caller-breakdown', lcallers, hcallers, ['parent', 'child']),
                                      ('allocation-callers', lalloc, halloc, ['heap_function', 'application_caller'])]:
        rows = []
        for pair in set(left) | set(right):
            rows.append(dict(zip(fields, pair)) | dict(legacy_ms_per_tick=f'{left[pair]/UNIT/TICKS:.9f}',
                        hyperpath_ms_per_tick=f'{right[pair]/UNIT/TICKS:.9f}', delta_ms_per_tick=f'{(right[pair]-left[pair])/UNIT/TICKS:.9f}'))
        rows.sort(key=lambda row: (-float(row['delta_ms_per_tick']), row[fields[0]], row[fields[1]]))
        write_csv(args.evidence / (name + '.csv'), list(rows[0]), rows)
    diff = []
    for scope in legacy:
        for function in set(legacy[scope]) | set(hyperpath[scope]):
            left, right = legacy[scope][function], hyperpath[scope][function]
            diff.append(dict(scope=scope, function=function, legacy_inclusive_ms_per_tick=f'{left[0]/UNIT/TICKS:.9f}',
                             hyperpath_inclusive_ms_per_tick=f'{right[0]/UNIT/TICKS:.9f}',
                             delta_inclusive_ms_per_tick=f'{(right[0]-left[0])/UNIT/TICKS:.9f}',
                             legacy_exclusive_ms_per_tick=f'{left[2]/UNIT/TICKS:.9f}',
                             hyperpath_exclusive_ms_per_tick=f'{right[2]/UNIT/TICKS:.9f}',
                             delta_exclusive_ms_per_tick=f'{(right[2]-left[2])/UNIT/TICKS:.9f}'))
    diff.sort(key=lambda r: (r['scope'], -float(r['delta_exclusive_ms_per_tick']), r['function']))
    write_csv(args.evidence / 'function-diff.csv', list(diff[0]), diff)
    branch_rows = []
    for branch in set(lbranches) | set(hbranches):
        left, right = lbranches[branch], hbranches[branch]
        branch_rows.append(dict(branch=branch, legacy_ms_per_tick=f'{left[0]/UNIT/TICKS:.9f}',
                                hyperpath_ms_per_tick=f'{right[0]/UNIT/TICKS:.9f}',
                                delta_ms_per_tick=f'{(right[0]-left[0])/UNIT/TICKS:.9f}', legacy_samples=left[1], hyperpath_samples=right[1]))
    branch_rows.sort(key=lambda r: -float(r['delta_ms_per_tick']))
    write_csv(args.evidence / 'branch-diff.csv', list(branch_rows[0]), branch_rows)
    summary = dict(legacy=lsummary, hyperpath=hsummary, branches=branch_rows,
                   delta_simulation_wall_ms_per_tick=hsummary['simulation_wall_ms_per_tick']-lsummary['simulation_wall_ms_per_tick'],
                   method='Inclusive: union of functions per sampled stack. Whole-thread exclusive: WPA Function column. simulation_stack exclusive: stack leaf. All times are sampled CPU except explicit wall timers.')
    dump(args.evidence / 'summary.json', summary)
    print(json.dumps(summary, indent=2))
    for kind in ['delta_inclusive_ms_per_tick', 'delta_exclusive_ms_per_tick']:
        print('\nTOP MAIN', kind)
        rows = sorted((r for r in diff if r['scope']=='main'), key=lambda r:-float(r[kind]))
        for row in rows[:16]:
            print(row[kind], row['function'][:180])


if __name__ == '__main__':
    main()
