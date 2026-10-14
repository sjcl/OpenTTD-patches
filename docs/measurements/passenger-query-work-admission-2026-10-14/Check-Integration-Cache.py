"""Compare actual logical accounting and transport, without a CPU comparison."""
import argparse
import importlib.util
import json
import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('profile', HERE.parent / 'passenger-routing-profile-2026-10-13/Summarize-Profile.py')
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('review', type=pathlib.Path)
a = p.parse_args()
records, texts = [], []
for name in ['warm-512', 'cold-scratch-off-512']:
    text = (a.review / name / 'on-1-console.txt').read_text(encoding='utf-8-sig')
    texts.append(text)
    records.append(profile.parse_console(text)['records'])
keys = ['Periodic admission', 'Periodic allocation', 'Periodic Work outcomes', 'Periodic missing',
        'Timed result states', 'Timed Work completion', 'Journey transport', 'Schedule transport',
        'Untimed transport', 'Completed interchanges', 'Passenger inventory', 'Waiting routing', 'Waiting age sample']
assert all(all(key in record for key in keys) for record in records)
checks = {key: records[0][key] == records[1][key] for key in keys}
checks['query_work'] = records[0]['Graph work']['query_allowance'] == records[1]['Graph work']['query_allowance']
checks['timed_logical_work'] = records[0]['Periodic departures']['search_work'] == records[1]['Periodic departures']['search_work']
checks['schedule_logical_work'] = records[0]['Schedule routing']['work'] == records[1]['Schedule routing']['work']
reasons = [dict(re.findall(r'^Admission rejected (\w+): (\d+)$', text, re.M)) for text in texts]
assert len(reasons[0]) == len(reasons[1]) == 6
checks['rejection_reasons'] = reasons[0] == reasons[1]
for prefix in ['Periodic tick searches', 'Periodic tick Work upper']:
    rows = [re.findall(r'^' + prefix + r' (\d+): (\d+)$', text, re.M) for text in texts]
    checks[prefix] = rows[0] == rows[1] and bool(rows[0])
admission = records[0]['Periodic admission']
checks['actual_searches_exercised'] = admission['accepted'] > 0 and admission['used'] > 0
checks['settled_work'] = admission['reserved'] == admission['used'] + admission['returned']
allocation = records[0]['Periodic allocation']
checks['planner_work_settled'] = admission['used'] == allocation['timed_used'] + allocation['schedule_used']
checks['query_work_charged'] = admission['used'] == records[0]['Graph work']['query_allowance']
checks['fixed_limits'] = admission['max_tick_queries'] <= 16 and admission['max_tick_work'] <= 1048576
checks['cold_controls_disabled'] = all(label + ': disabled' in texts[1] for label in
                                      ['Periodic policy residency', 'Periodic departure index', 'OD fallback residency', 'Timed search scratch'])
checks['policy_cache_exercised'] = records[0]['Periodic database']['policy_hits'] > 0 and records[1]['Periodic database']['policy_hits'] == 0
checks['scratch_exercised'] = records[0]['Timed search storage']['scratch_bytes'] > 0 and records[1]['Timed search storage']['scratch_bytes'] == 0
result = {'checks': checks, 'records': [{key: record[key] for key in keys} for record in records],
          'rejections': reasons, 'performance_comparison': 'skipped by user',
          'note': 'Full-save SHA and all Packet/payment fields are checked separately by Run-Integration-SaveLoad.py.'}
(a.review / 'cache-determinism.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
if not all(checks.values()):
    raise ValueError(checks)
print(json.dumps(checks, indent=2))
