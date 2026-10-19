from pathlib import Path
import ast, hashlib, json
ROOT = Path(r'C:\Users\bebe\.codex\worktrees\6d2b\OpenTTD-patches')
BASE = ROOT / 'build-compiler-comparison'
EVIDENCE = ROOT / 'docs/measurements/compiler-release-real-map-2026-10-19'
def read(path): return json.loads(path.read_text(encoding='utf-8'))
def sha(path):
    with path.open('rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()
identity = read(EVIDENCE/'build-identity.json')
assert sha(Path(identity['original_input'])) == sha(BASE/'input/input.sav') == identity['input_sha256']
summary = read(BASE/'summary.json')
assert all(not value for value in summary['mismatches'].values())
for name in ('wpr-msvc-1','wpr-clang-1','wpr-clang-2','wpr-msvc-2'):
    folder = BASE/'runs'/name
    result = read(folder/'process.json')
    clock = result['actual-ticks.json']
    assert [row[0] for row in clock['samples']] == list(range(clock['start_tick']+1,clock['end_tick']+1))
    assert len(clock['samples']) == 1024
    assert result['binary_sha256'] == sha(BASE/name.split('-')[1]/'Release/openttd.exe')
    assert result['simulation_timing_valid'] and result['warm_complete'] and result['exit_code'] == 0
    stats = (folder/'trace-stats.txt').read_text(encoding='utf-8-sig')
    assert 'Total # Lost Buffers : 0' in stats and 'Total # Lost Events  : 0' in stats
mp = [read(BASE/name/'result.json') for name in ('mp-msvc-clang','mp-clang-msvc')]
for result in mp:
    assert result['pass'] and result['joined'] and not result['network_error']
    assert result['server_snapshot'] == result['client_snapshot']
    assert result['transport_server'] == result['transport_client']
    assert result['actual_ticks'] == 288
    assert result['replay']['start_tick'] == result['replay']['end_tick']
assert mp[0]['transport_server'] == mp[1]['transport_server']
assert identity['multiplayer_complete']
for name in ('utils/passenger_precomputed_real_map.py','utils/passenger_precomputed_join.py'):
    ast.parse((ROOT/name).read_text(encoding='utf-8'))
assert '<!-- MULTIPLAYER_RESULTS -->' not in (ROOT/'docs/compiler-release-real-map-2026-10-19.md').read_text(encoding='utf-8')
print('PASS: original input unchanged, binary hashes, consecutive ticks, zero lost events, all canonical states, both MP directions, replay tick barriers, Python syntax')
