"""Summarize repeated resumes and match the initially rejected depot services."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('run', type=pathlib.Path)
p.add_argument('output', type=pathlib.Path)
a = p.parse_args()
labels = ['depot-before', 'depot-after', 'depot-before2', 'depot-after2']
summarizer = pathlib.Path(__file__).resolve().parent.parent / 'passenger-journey-periodic-2026-10-12/Summarize-Measurements.py'
subprocess.run([sys.executable, str(summarizer), str(a.run), str(a.output), *labels], check=True, stdout=subprocess.DEVNULL)
summary = json.loads((a.output / 'summary.json').read_text())
for label, data in summary.items():
    text = (a.run / f'{label}-console.txt').read_text(encoding='utf-8')
    for prefix, key in [('Periodic operating events:', 'operating'), ('Timed depot transport:', 'depot_transport')]:
        rows = [dict((k, int(v)) for k, v in re.findall(r'(\w+)=(\d+)', line)) for line in text.splitlines() if line.startswith(prefix)]
        data[key] = rows[-1] if rows else None
    assert len(data['state_ticks']) == 2 and data['state_ticks'][-1] > data['state_ticks'][0]
    assert data['simulation']['completed_ticks'] == data['state_ticks'][-1] - data['state_ticks'][0] - 1

def logical(data):
    return {key: value for key, value in data.items() if key in ['state_ticks', 'timed_transport', 'untimed_transport', 'operating', 'depot_transport', 'fallback_reasons', 'global_invalidations']}
assert logical(summary['depot-before']) == logical(summary['depot-before2'])
assert logical(summary['depot-after']) == logical(summary['depot-after2'])
assert len({data['process']['input_sha256'] for data in summary.values()}) == 1

def audit(name):
    path = a.run / f'{name}-audit.txt'
    services = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        if m := re.fullmatch(r'Depot service (\d+): (.*)', line):
            services[int(m[1])] = dict(field.split('=', 1) for field in m[2].split('; '))
    (a.output / path.name).write_bytes(path.read_bytes())
    return services

start = audit('depot-fixed-start')
finish = audit('depot-fixed-finish')
eligible = [ident for ident, data in start.items() if int(data['passenger_vehicles']) > 0 and data['live_period'] != '0']
promoted = [ident for ident in eligible if ident in finish and finish[ident]['published_period'] != '0']
comparison = {
    'initial_depot_lists': len(start),
    'initial_passenger_depot_lists': sum(int(data['passenger_vehicles']) > 0 for data in start.values()),
    'initial_eligible_passenger_depot_lists': len(eligible),
    'formerly_untimed_lists_published_timed': len(promoted),
    'promoted_service_ids': promoted,
    'not_yet_promoted_service_ids': sorted(set(eligible) - set(promoted)),
    'paired_means_ms': {},
}
for group in ['before', 'after']:
    runs = [summary[f'depot-{group}{suffix}'] for suffix in ['', '2']]
    comparison['paired_means_ms'][group] = {
        'simulation': sum(data['simulation']['total_us'] / data['simulation']['completed_ticks'] for data in runs) / 2000,
        'journey': sum(data['histogram']['journey_mean_us'] for data in runs) / 2000,
    }
for key in ['simulation', 'journey']:
    comparison.setdefault('mean_change_percent', {})[key] = 100 * (comparison['paired_means_ms']['after'][key] / comparison['paired_means_ms']['before'][key] - 1)

(a.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
(a.output / 'comparison.json').write_text(json.dumps(comparison, indent=2) + '\n', encoding='utf-8')
print(json.dumps(comparison, indent=2))
