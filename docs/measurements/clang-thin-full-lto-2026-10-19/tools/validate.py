from pathlib import Path
import hashlib,json,ast
ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'build-clang-lto-comparison'
EVIDENCE=ROOT/'docs/measurements/clang-thin-full-lto-2026-10-19'
def read(p):return json.loads(p.read_text(encoding='utf-8'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
identity=read(EVIDENCE/'build-identity.json')
assert sha(Path(identity['original_input']))==sha(BASE/'input/input.sav')==identity['input_sha256']
assert identity['builds']['thin']['revision']==identity['builds']['full']['revision']
for mode in ('thin','full'):
    for data in identity['builds'][mode]['objects'].values():assert data['first_32_bytes'].startswith('4243c0de')
    overlay=(BASE/mode/'measurement.cmake').read_text()
    assert '/Z7' in identity['builds'][mode]['flags']
    symbols=(BASE/mode/'pdb-scope-check.txt').read_text(encoding='utf-8-sig')
    assert all(name in symbols for name in ('StateGameLoop','PassengerJourneyLoadingContext::CanBoard','PassengerServiceDatabase::PlanPrecomputed'))
    assert ('-flto=thin' in overlay)==(mode=='thin')
    assert ('-flto>' in overlay)==(mode=='full')
    assert f'-flto={mode}' in (BASE/mode/'driver-mode.txt').read_text(encoding='utf-8-sig')
    for file in identity['builds'][mode]['copied_tus']:
        a=(BASE/'thin'/file).read_text(encoding='utf-8').replace('build-clang-lto-comparison/thin','BUILD')
        b=(BASE/'full'/file).read_text(encoding='utf-8').replace('build-clang-lto-comparison/full','BUILD')
        assert a==b,file
summary=read(BASE/'summary.json')
assert all(not value for value in summary['mismatches'].values())
old=read(ROOT/'docs/measurements/compiler-lto-real-map-2026-10-19/summary.json')
assert summary['canonical']['wpr-thin-1']==old['canonical']['wpr-clang-1']
assert [r['name'] for r in summary['runs']]==['wpr-thin-1','wpr-full-1','wpr-full-2']
assert read(EVIDENCE/'exclusions.json')[0]['lost_events']==1449719
for name in (r['name'] for r in summary['runs']):
    directory=BASE/'runs'/name;r=read(directory/'process.json');clock=r['actual-ticks.json'];mode=name.split('-')[1]
    assert clock['start_tick']==3197276448 and clock['end_tick']==3197277472
    assert [x[0] for x in clock['samples']]==list(range(3197276449,3197277473))
    assert r['exit_code']==0 and r['simulation_timing_valid'] and r['warm_complete'] and r['save-integrity.json']['payments_valid']
    assert r['binary_sha256']==sha(BASE/mode/'Release/openttd.exe')==identity['builds'][mode]['exe_sha256']
    stats=(directory/'trace-stats.txt').read_text(encoding='utf-8-sig')
    assert 'Total # Lost Buffers : 0' in stats and 'Total # Lost Events  : 0' in stats
results=[]
for server,client in (('thin','full'),('full','thin')):
    r=read(BASE/f'mp-{server}-{client}/result.json');results.append(r)
    assert r['pass'] and r['joined'] and not r['network_error']
    assert r['server_snapshot']==r['client_snapshot'] and r['transport_server']==r['transport_client']
    assert r['server_snapshot']['state_ticks']==3197276448 and r['actual_ticks']>=256
    assert r['transport_server']['state_ticks']-r['server_snapshot']['state_ticks']==r['actual_ticks']
    assert r['replay']['start_tick']==r['replay']['end_tick']==3197276448
    assert r['server_binary_sha256']==identity['builds'][server]['exe_sha256']
    assert r['client_binary_sha256']==identity['builds'][client]['exe_sha256']
assert results[0]['server_snapshot']==results[1]['server_snapshot']
assert identity['multiplayer_complete']
for p in (EVIDENCE/'tools').glob('*.py'):ast.parse(p.read_text(encoding='utf-8'))
report=(ROOT/'docs/clang-thin-full-lto-2026-10-19.md').read_text(encoding='utf-8')
assert '<!-- PERFORMANCE_RESULTS -->' not in report and '<!-- MULTIPLAYER_RESULTS -->' not in report
print('PASS: unchanged original, identical source/revisions, LTO modes, exact ticks, CPU trace integrity, canonical states including reference, both multiplayer directions and binary identities')
