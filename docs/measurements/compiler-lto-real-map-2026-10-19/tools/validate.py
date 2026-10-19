from pathlib import Path
import ast, hashlib, json
ROOT = Path(r'C:\Users\bebe\.codex\worktrees\6d2b\OpenTTD-patches')
BASE = ROOT / 'build-compiler-lto-comparison'
EVIDENCE = ROOT / 'docs/measurements/compiler-lto-real-map-2026-10-19'
def read(path): return json.loads(path.read_text(encoding='utf-8'))
def sha(path):
    with path.open('rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()
identity = read(EVIDENCE/'build-identity.json')
assert identity['compiler_versions']['msvc'] == '19.51.36260.0'
assert all(p['actual_sha256'] == p['payload']['sha256'] for p in identity['isolated_msvc_acquisition'])
assert sha(Path(identity['original_input'])) == sha(BASE/'input/input.sav') == identity['input_sha256']
summary = read(BASE/'summary.json')
assert all(not value for value in summary['mismatches'].values())
old = read(ROOT/'docs/measurements/compiler-release-real-map-2026-10-19/summary.json')
assert summary['canonical']['wpr-msvc-1'] == old['canonical']['wpr-msvc-1']
for name in ('wpr-msvc-1','wpr-clang-1','wpr-clang-2','wpr-msvc-2'):
    folder = BASE/'runs'/name
    result = read(folder/'process.json')
    clock = result['actual-ticks.json']
    assert [row[0] for row in clock['samples']] == list(range(clock['start_tick']+1,clock['end_tick']+1))
    assert len(clock['samples']) == 1024
    assert clock['start_tick'] == 3197276448 and clock['end_tick'] == 3197277472
    assert result['binary_sha256'] == sha(BASE/name.split('-')[1]/'Release/openttd.exe')
    assert result['simulation_timing_valid'] and result['warm_complete'] and result['exit_code'] == 0
    stats = (folder/'trace-stats.txt').read_text(encoding='utf-8-sig')
    assert 'Total # Lost Buffers : 0' in stats and 'Total # Lost Events  : 0' in stats
mp = [read(BASE/name/'result.json') for name in ('mp-msvc-clang','mp-clang-msvc')]
for result, (server, client) in zip(mp, (('msvc','clang'),('clang','msvc'))):
    assert result['server_binary_sha256'] == identity['builds'][server]['exe_sha256']
    assert result['client_binary_sha256'] == identity['builds'][client]['exe_sha256']
for result in mp:
    assert result['pass'] and result['joined'] and not result['network_error']
    assert result['server_snapshot'] == result['client_snapshot']
    assert result['transport_server'] == result['transport_client']
    # This harness polls every 2 s and distributes pause at a future network
    # frame. Its multiplayer endpoint is observed, not a fixed 288-tick bound.
    assert result['actual_ticks'] >= 256
    assert result['server_snapshot']['state_ticks'] == 3197276448
    assert result['transport_server']['state_ticks'] - result['server_snapshot']['state_ticks'] == result['actual_ticks']
    assert result['replay']['start_tick'] == result['replay']['end_tick']
old_mp = read(ROOT/'docs/measurements/compiler-release-real-map-2026-10-19/mp-msvc-clang/result.json')
assert mp[0]['server_snapshot'] == mp[1]['server_snapshot'] == old_mp['server_snapshot']
for result in mp:
    if result['transport_server']['state_ticks'] == old_mp['transport_server']['state_ticks']:
        assert result['transport_server'] == old_mp['transport_server']
assert identity['multiplayer_complete']
for name in ('utils/passenger_precomputed_real_map.py','utils/passenger_precomputed_join.py'):
    ast.parse((ROOT/name).read_text(encoding='utf-8'))
report = (ROOT/'docs/compiler-lto-real-map-2026-10-19.md').read_text(encoding='utf-8')
assert '<!-- PERFORMANCE_RESULTS -->' not in report and '<!-- CORRECTNESS_RESULTS -->' not in report
print('PASS: original input unchanged, binary hashes, exact consecutive performance ticks, zero lost events, all canonical performance states including non-LTO reference, both MP directions and initial non-LTO reference, replay tick barriers, Python syntax')
