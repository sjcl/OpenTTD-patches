import collections, importlib.util, json, math, pathlib, re, statistics, sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build-rounds'

def fields(line):
    def convert(value):
        if value in ('true', 'false'): return value == 'true'
        try: return int(value)
        except ValueError: return value
    return {k: convert(v) for k, v in re.findall(r'(\w+)=([^;\s]+)', line)}

def percentile(values, p):
    return sorted(values)[max(0, math.ceil(len(values) * p / 100) - 1)] if values else None

def quality(path):
    rows, context = [], {}
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        if line.startswith('Timed comparison input:'): context = fields(line)
        elif line.startswith('Timed comparison OD:'):
            rows.append({**context, **fields(line), 'legs': []})
        elif line.startswith('Timed comparison work:'): rows[-1]['detail'] = fields(line)
        elif line.startswith('Timed comparison leg:'): rows[-1]['legs'].append(fields(line))
    found = lambda r: r['round_outcome'] in ('Found', 'Provisional')
    eligible = lambda r: r['reference_found'] and not r['reference_deferred'] and r['reference_executable']
    compared = [r for r in rows if found(r) and eligible(r)]
    diffs = [r['round_cost'] - r['reference_cost'] for r in compared]
    ratios = [(r['round_cost'] / r['reference_cost'] - 1) for r in compared if r['reference_cost'] > 0]
    loops, invalid = [], []
    for r in rows:
        stations = [str(l['alight']).split('/')[0] for l in r['legs'][:-1]]
        if len(stations) != len(set(stations)): loops.append(r)
        for a, b in zip(r['legs'], r['legs'][1:]):
            if str(a['alight']).split('/')[0] != str(b['board']).split('/')[0] or a['service'] == b['service'] or b['departure'] < a['arrival']: invalid.append(r); break
    result = {'source': str(path), 'sample': len(rows), 'tick': sorted({r['tick'] for r in rows}),
              'outcomes': dict(collections.Counter(r['round_outcome'] for r in rows)),
              'legacy_found': sum(r['legacy_found'] for r in rows),
              'legacy_deferred': sum(r['legacy_deferred'] for r in rows),
              'reference_found': sum(r['reference_found'] for r in rows),
              'reference_deferred': sum(r['reference_deferred'] for r in rows),
              'non_executable_reference': sum(r['reference_found'] and not r['reference_executable'] for r in rows),
              'comparable': len(compared), 'same_cost': sum(d == 0 for d in diffs), 'better_cost': sum(d < 0 for d in diffs),
              'worse_cost': sum(d > 0 for d in diffs), 'max_extra_cost': max(diffs) if diffs else None,
              'cost_relative_p95': percentile(ratios, 95), 'cost_relative_max': max(ratios) if ratios else None,
              'reference_found_round_missing': sum(eligible(r) and not found(r) for r in rows),
              'round_found_reference_missing': sum(found(r) and not r['reference_found'] for r in rows),
              'legacy_work_median': statistics.median(r['legacy_work'] for r in rows),
              'round_work_median': statistics.median(r['round_work'] for r in rows),
              'legacy_labels_mean': statistics.mean(r['legacy_labels'] for r in rows),
              'round_labels_mean': statistics.mean(r['round_labels'] for r in rows),
              'legacy_elapsed_us_mean': statistics.mean(r['legacy_us'] for r in rows),
              'round_elapsed_us_mean': statistics.mean(r['round_us'] for r in rows),
              'repeated_transfer_station_queries': len(loops), 'inconsistent_leg_queries': len(invalid),
              'first_service_queries': dict(collections.Counter(str(r['legs'][0]['service']) for r in rows if r['legs'])),
              'first_wait_ticks_p95': percentile([r['legs'][0]['departure'] - r['ready'] for r in rows if r['legs']], 95),
              'first_wait_ticks_max': max((r['legs'][0]['departure'] - r['ready'] for r in rows if r['legs']), default=None),
              'worse_queries': [r for r in compared if r['round_cost'] > r['reference_cost']],
              'missed_queries': [r for r in rows if eligible(r) and not found(r)]}
    return result

if __name__ == '__main__':
    source = pathlib.Path(sys.argv[1]); output = pathlib.Path(sys.argv[2])
    output.write_text(json.dumps(quality(source), indent=2) + '\n', encoding='utf-8')
    print(output)
