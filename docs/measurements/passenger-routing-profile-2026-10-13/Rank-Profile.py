"""Rank disjoint coarse Journey scopes from valid minimal cache-on observations.

Detailed children are supplementary inclusive measurements. Never infer missing
CPU attribution from call counts. Elapsed scopes are not sampled CPU percentages.
"""
import argparse
import json
import pathlib
import statistics


def rank(data):
    groups = {}
    for run in data['runs']:
        if run['valid'] and run['kind'] == 'minimal' and run['mode'] == 'on':
            groups.setdefault((run['label'], run['phase']), []).append(run)
    result = []
    for (label, phase), runs in groups.items():
        costs = []
        for run in runs:
            d = run['derived']
            times = d['time_ms_per_tick']
            needed = ['timed_forward_and_cache_other', 'timed_reverse_cold', 'schedule_query_inclusive', 'database_tick_inclusive']
            if any(times.get(key) is None for key in needed) or d['journey_mean_ms'] is None:
                continue
            row = {key: times[key] for key in needed}
            row['remaining_journey'] = d['journey_mean_ms'] - sum(row.values())
            if row['remaining_journey'] < 0:
                continue
            costs.append(row)
        entries = sorted(({'scope': key, 'mean_ms_per_tick': statistics.mean(c[key] for c in costs)}
                          for key in costs[0]), key=lambda e: e['mean_ms_per_tick'], reverse=True) if costs else []
        for i, entry in enumerate(entries):
            entry['rank'] = i + 1
        result.append({'label': label, 'phase': phase, 'valid_runs': len(costs), 'ranking': entries,
                       'attribution': 'existing steady_clock elapsed scopes on simulation thread; corroborate fine-grained targets with detail/sampling',
                       'remaining_journey_note': 'Residual of inclusive Journey minus disjoint top-level scopes, not a named function or exclusive CPU sample.'})
    return {'schema_version': 1, 'status': 'measured' if any(row['ranking'] for row in result) else 'not_measured', 'results': result,
            'notes': ['NextDeparture call count alone does not establish CPU share.',
                      'Detail timers and counters perturb hot loops and never enter this ranking.',
                      'Database Tick includes publication time; publication_us is a child, not a disjoint ranked entry.',
                      'Fallback Distance and waiting/boarding scopes overlap the Journey parents and are not added as disjoint entries.']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('summary', type=pathlib.Path)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    result = rank(json.loads(args.summary.read_text(encoding='utf-8')))
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False, indent=2))
