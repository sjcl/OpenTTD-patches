import argparse, json, pathlib, statistics

BUILD = pathlib.Path(__file__).resolve().parent

def metrics(run):
    records, derived = run['parsed']['records'], run['derived']
    db = records['Periodic database']
    query_cpu = records['Periodic query CPU (OS thread clock; aggregate only)']
    detail = records['Legacy timed detail'] if run['method'] == 'legacy' else records['Timed rounds']
    timed, schedule, untimed = [records[k] for k in ['Journey transport', 'Schedule transport', 'Untimed transport']]
    outcomes = run['parsed']['periodic_outcomes']
    work = outcomes['Work']['outcomes']
    return {
        'simulation_ms': derived['simulation_mean_ms'],
        'simulation_p95_upper_ms': derived['quantiles']['simulation']['p95']['upper_us'] / 1000,
        'simulation_p99_upper_ms': derived['quantiles']['simulation']['p99']['upper_us'] / 1000,
        'journey_ms': derived['journey_mean_ms'], 'process_cpu_seconds': run['process']['cpu_seconds'],
        'queries': db['searches'], 'query_elapsed_us': db['query_us'],
        'query_elapsed_us_per_od': db['query_us'] / db['searches'],
        'query_cpu_ns': query_cpu['query_cpu_ns'], 'query_cpu_samples': query_cpu['samples'],
        'query_cpu_ms_per_od': query_cpu['query_cpu_ns'] / db['searches'] / 1e6,
        'labels': records['Timed search storage']['labels'],
        'label_attempts': detail['attempts'], 'label_comparisons': detail['comparisons'],
        'onboard_comparisons': detail.get('onboard_comparisons', 0),
        'service_candidates': detail['service_candidates'], 'service_scans': detail['service_scans'],
        'work_stops': work, 'work_stop_fraction': work / db['searches'],
        'timed_applied_people': records['Active journey total']['applied_passengers'],
        'timed_boarded': timed['boarded'], 'timed_transferred': timed['transferred'], 'timed_delivered': timed['delivered'],
        'schedule_boarded': schedule['boarded'], 'schedule_delivered': schedule['delivered'],
        'untimed_boarded': untimed['boarded'], 'untimed_delivered': untimed['delivered'],
        'total_delivered': timed['delivered'] + schedule['delivered'] + untimed['delivered'],
        'peak_working_set': run['process']['peak_working_set'], 'sampled_peak_private': run['process']['sampled_peak_private'],
        'round_peak_logical_memory': records['Timed rounds']['peak_memory'] if run['method'] == 'rounds' else None,
        'expired_packet_observations': records['Periodic execution observations']['expired'],
        'failed_same_service_boarding_checks': records['Periodic execution observations']['failed_same_service_boarding_checks'],
    }

if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('manifest', type=pathlib.Path); parser.add_argument('output', type=pathlib.Path); args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
    groups = {}
    for method in ['legacy', 'rounds']:
        runs = [r for r in manifest['runs'] if r['method'] == method]
        data = [metrics(r) for r in runs]
        if not data: continue
        groups[method] = {'runs': len(runs), 'mean': {k: statistics.mean(d[k] for d in data) if data[0][k] is not None else None for k in data[0]},
                          'range': {k: [min(d[k] for d in data), max(d[k] for d in data)] if data[0][k] is not None else None for k in data[0]},
                          'save_hashes': [r['final_save_sha256'] for r in runs],
                          'repeat_save_identical': len({r['final_save_sha256'] for r in runs}) == 1,
                          'round_outcomes': [r['parsed']['round_outcomes'] for r in runs]}
    comparisons = {k: groups['rounds']['mean'][k] / groups['legacy']['mean'][k] - 1
                   for k in groups.get('legacy', {}).get('mean', {})
                   if 'rounds' in groups and groups['legacy']['mean'][k] and groups['rounds']['mean'][k] is not None}
    result = {'status': manifest['status'], 'groups': groups, 'relative_changes': comparisons}
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
