"""Archive corrected six-run evidence and check ordered cache determinism."""
import argparse
import gzip
import hashlib
import importlib.util
import json
import pathlib
import re
import shutil
import statistics

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('admission', HERE / 'Summarize.py')
admission = importlib.util.module_from_spec(spec)
spec.loader.exec_module(admission)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('sequence', type=pathlib.Path)
    p.add_argument('--archive', type=pathlib.Path, required=True)
    a = p.parse_args()
    sequence = json.loads(a.sequence.read_text(encoding='utf-8'))
    if sequence['status'] != 'complete' or len(sequence['manifests']) != 6:
        raise ValueError('Six completed runs required')
    output = a.archive
    output.mkdir(exist_ok=True)
    runs = []
    keys = ['Periodic admission', 'Periodic allocation', 'Periodic Work outcomes', 'Periodic missing',
        'Timed result states', 'Timed Work completion', 'Schedule routing', 'Journey transport', 'Schedule transport', 'Untimed transport',
        'Completed interchanges', 'Passenger inventory', 'Waiting routing', 'Waiting age sample', 'Final measurement', 'Final old admission']
    for path in map(pathlib.Path, sequence['manifests']):
        manifest, records = admission.profile.read_manifest(path)
        run = records[0]
        if not run['valid']:
            raise ValueError(run['errors'])
        admission.check_accounting(run)
        raw = run['diagnostics']['records']
        measurements = raw['Final measurement']
        if measurements['cpu_failures'] or raw['Final old admission']['peak_tick_queries'] > 16 or raw['Final old admission']['peak_tick_work'] > 1048576:
            raise ValueError('Probe failure or budget limit exceeded')
        condition = 'A' if manifest['label'] == 'control' else ('C' if run['mode'] == 'cold' else 'B')
        run['condition'] = condition
        run['balanced_sequence_index'] = manifest['balanced_sequence_index']
        run['metrics'] = {key: raw.get(key) for key in keys}
        run['total_delivered'] = sum(raw[key]['delivered'] for key in ['Journey transport', 'Schedule transport', 'Untimed transport'])
        run['timed_cpu_seconds'] = measurements['timed_cpu_100ns'] / 10000000
        run['final_save_bytes'] = manifest['runs'][0]['final_save_bytes']
        if condition == 'A':
            old = raw['Final old admission']
            legacy_rescue_inferred_zero = raw['Schedule routing']['timed_deferred'] == 0
            run['accounting'] = {'accepted': old['accepted'], 'rejected': old['rejected_queries'] + old['rejected_work'],
                'rejections': {'QueryLimit': old['rejected_queries'], 'QueryWork': old['rejected_work']},
                'reserved': old['accepted'] * 65536, 'charged': old['accepted'] * 65536, 'returned': 0,
                'logical_search_work': raw['Periodic departures']['search_work'] + raw['Schedule routing']['work'],
                'max_tick_queries': old['peak_tick_queries'], 'max_tick_charged_work': old['peak_tick_work'],
                'work_no_route': raw['Timed Work completion']['without_complete'] if legacy_rescue_inferred_zero else None,
                'work_schedule_rescued': 0 if legacy_rescue_inferred_zero else None,
                'work_no_route_derivation': 'Work without complete equals final no-route here: Schedule timed_deferred packets are zero.' if legacy_rescue_inferred_zero else
                    'Legacy Schedule deferred packets are nonzero; packet counts cannot infer unique Work-stopped OD rescues or final no-route.'}
        else:
            console = (path.parent / run['directory'] / (run['id'] + '-console.txt')).read_text(encoding='utf-8-sig')
            rejections = {reason: int(count) for reason, count in re.findall(r'^Admission rejected (\w+): (\d+)$', console, re.M)}
            if set(rejections) != {'EventQueries', 'TickQueries', 'EventWork', 'GlobalWork', 'FairWork', 'FairQueries'}:
                raise ValueError('Missing admission rejection reasons')
            if sum(rejections.values()) != raw['Periodic admission']['rejected']:
                raise ValueError('Admission rejection total differs')
            run['accounting'] = dict(raw['Periodic admission'], rejections=rejections)
        target = output / f"{manifest['balanced_sequence_index']:02}-{condition}"
        target.mkdir(exist_ok=True)
        directory = path.parent / run['directory']
        for suffix, name in [('-console.txt', 'console.txt'), ('-process.json', 'process.json'), ('-stderr.txt', 'stderr.txt')]:
            source = directory / (run['id'] + suffix)
            if name == 'stderr.txt':
                data = source.read_bytes()
                (target / 'stderr.txt.gz').write_bytes(gzip.compress(data, mtime=0))
                # This is only the explicitly owned archive copy. Full bytes
                # remain in the gzip, and the original run is left intact.
                if (target / name).exists():
                    (target / name).unlink()
                run['archived_stderr'] = {'file': 'stderr.txt.gz', 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            else:
                shutil.copy2(source, target / name)
        (target / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        (target / 'validation.json').write_text(json.dumps(run['evidence'], indent=2) + '\n', encoding='utf-8')
        runs.append(run)
    for index, path in enumerate(map(pathlib.Path, sequence['excluded']), 1):
        manifest = json.loads(path.read_text(encoding='utf-8'))
        target = output / f'excluded-{index:02}'
        target.mkdir(exist_ok=True)
        shutil.copy2(path, target / 'manifest.json')
        for failed_run in manifest.get('runs', []):
            directory = path.parent / failed_run['directory']
            for suffix in ['-console.txt', '-process.json', '-stderr.txt']:
                source = directory / (failed_run['id'] + suffix)
                if not source.exists():
                    continue
                if suffix == '-stderr.txt':
                    (target / (source.name + '.gz')).write_bytes(gzip.compress(source.read_bytes(), mtime=0))
                else:
                    shutil.copy2(source, target / source.name)
    fixed = [run for run in runs if run['condition'] != 'A']
    digest_keys = ['query_count', 'query_digest', 'result_count', 'result_digest', 'admission_count', 'admission_digest', 'packet_current', 'packet_peak']
    baseline = fixed[0]
    checks = {key: all(run['metrics']['Final measurement'][key] == baseline['metrics']['Final measurement'][key] for run in fixed) for key in digest_keys}
    checks['full_save_sha256'] = all(run['final_save_sha256'] == baseline['final_save_sha256'] for run in fixed)
    checks['semantic_signature'] = all(run['semantic_signature'] == baseline['semantic_signature'] for run in fixed)
    checks['accounting'] = all(run['metrics'][key] == baseline['metrics'][key] for run in fixed for key in ['Periodic admission', 'Periodic allocation', 'Periodic Work outcomes', 'Periodic missing', 'Waiting age sample'])
    checks['rejection_reasons'] = all(run['accounting']['rejections'] == baseline['accounting']['rejections'] for run in fixed)
    if not all(checks.values()):
        raise ValueError('Cache determinism mismatch: ' + str(checks))
    groups = {}
    for condition in ['A', 'B', 'C']:
        selected = [run for run in runs if run['condition'] == condition]
        means = [run['derived']['simulation_mean_ms'] for run in selected]
        groups[condition] = {'runs': len(selected), 'simulation_mean_ms': statistics.mean(means),
            'simulation_mean_range_ms': [min(means), max(means)], 'simulation_mean_spread_percent': 100 * (max(means) - min(means)) / statistics.mean(means),
            'total_delivered': [run['total_delivered'] for run in selected], 'packet_peak': [run['metrics']['Final measurement']['packet_peak'] for run in selected]}
    revisions = {run['revision'] for run in runs}
    if len(revisions) != 1:
        raise ValueError('Different product source revisions in one comparison')
    control = json.loads(pathlib.Path(sequence['manifests'][0]).read_text(encoding='utf-8'))['instrumentation']['control_commit']
    result = {'source_revision': next(iter(revisions)), 'control_commit': control, 'sequence': sequence, 'runs': runs,
        'groups': groups, 'cache_determinism': checks, 'simulation_B_over_A': groups['B']['simulation_mean_ms'] / groups['A']['simulation_mean_ms'],
        'notes': [f'A uses the exact {control} ActiveContext with common read-only probes; other product sources come from the recorded merged source revision.',
            'GetThreadTimes per-call accounting is coarse; QueryThreadCycleTime and elapsed planner time are retained separately.',
            'Histograms provide 1ms upper bounds; two repetitions do not provide a general confidence interval.',
            'No retired intent_wait_ge_4096 or Work-cohort transport metric is used.']}
    (output / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'groups': groups, 'cache_determinism': checks, 'simulation_B_over_A': result['simulation_B_over_A']}, indent=2))


if __name__ == '__main__':
    main()
