"""Require paired uncontaminated cause evidence; keep excluded trials separate."""
import json
import pathlib

root = pathlib.Path(__file__).resolve().parent / 'network-cause-review'
formal = {}
for name in ['cold-1', 'cold-3', 'warm-2']:
    metadata = json.loads((root / name / 'run-metadata.json').read_text())
    analysis = json.loads((root / name / 'timeline-analysis.json').read_text())
    assert not metadata['contamination'], name
    formal[name] = {'metadata': metadata, 'analysis': analysis}
assert len({item['metadata']['binary_sha256'] for item in formal.values()}) == 1
assert len({item['metadata']['input_sha256'] for item in formal.values()}) == 1
assert formal['cold-1']['metadata']['input_sha256'] == '14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390'
for name in ['cold-1', 'cold-3']:
    assert formal[name]['metadata']['outcome'] == 'fail'
    findings = formal[name]['analysis']['findings']
    assert findings and all(f['classification'] == 'client_cpu_catchup_with_pending_data' for f in findings)
    assert all(f['catchup_game_thread_cpu_to_wall'] > .9 and f['pending_socket_bytes_before_close'] > 0 and
               f['server_os_accepted_bytes_lower_bound_during_catchup'] > 0 for f in findings)
warm = json.loads((root / 'warm-2/result.json').read_text())
assert formal['warm-2']['metadata']['outcome'] == 'pass'
assert formal['warm-2']['metadata']['requested_post_join_seconds'] == 120
assert warm['connected_clients_at_end'] == 2 and warm['cargo_state_equal'] and not warm['detected_sync_failure']
assert warm['client_cache_mode'] == 'warm' and warm['sync_frequency'] == 1
assert all(warm[field] for field in ['client_policy_cache_enabled', 'client_departure_index_enabled', 'client_fallback_cache_enabled', 'client_scratch_enabled'])
assert not formal['warm-2']['analysis']['findings']
formal['warm-2']['result'] = warm
excluded = {}
for name in ['cold-2', 'warm']:
    metadata = json.loads((root / name / 'run-metadata.json').read_text())
    assert metadata['contamination'], name
    excluded[name] = {'reason': 'host contention; excluded from formal CPU comparison', 'metadata': metadata}
result = {'formal': formal, 'excluded': excluded, 'timeout_extended': False, 'product_network_code_changed': False,
    'conclusion': 'Server continued sending; client CPU catch-up prevented receive re-entry until the unchanged timeout closed a socket containing pending bytes.',
    'integration_approved': False,
    'limits': ['All-off vs all-on does not separate the individual cache and scratch costs.',
        'The existing catch-up mechanism is unchanged from 8a96044d40, but old Admission was not tested in this network comparison.',
        'Timing instrumentation has overhead; these values are not reused as product performance benchmark results.']}
(root / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print('Cause checks PASS: 2 uncontaminated cold failures with server sends/pending data/CPU; warm 120s PASS; 2 contaminated trials excluded')
