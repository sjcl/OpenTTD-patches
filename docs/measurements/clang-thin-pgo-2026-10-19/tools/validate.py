from pathlib import Path
import json,hashlib,gzip,ast,re,xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'build-clang-thin-pgo';OUT=ROOT/'docs/measurements/clang-thin-pgo-2026-10-19'
def read(p):return json.loads(p.read_text(encoding='utf-8'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
i=read(OUT/'build-identity.json')
assert i['input_sha256']==sha(BASE/'input/input.sav')==sha(Path(i['original_input']))=='916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a'
assert i['profile_sha256']==sha(BASE/'profiles/real-map.profdata')==read(BASE/'profiles/identity.json')['profile_sha256']
assert hashlib.sha256(gzip.decompress((OUT/'profiles/real-map.profdata.gz').read_bytes())).hexdigest()==i['profile_sha256']
assert sha(BASE/'pgo/pgo'/(i['profile_sha256']+'.profdata'))==i['profile_sha256']
assert len({b['revision'] for b in i['builds'].values()})==1
for mode,value in i['builds'].items():
    assert sha(BASE/mode/'Release/openttd.exe')==value['exe_sha256']
    for project in ('openttd','openttd_lib'):
        root=ET.parse(BASE/mode/(project+'.vcxproj')).getroot()
        ns={'v':'http://schemas.microsoft.com/developer/msbuild/2003'}
        release_seen=False
        for group in root.findall('v:ItemDefinitionGroup',ns):
            opts=group.find('v:ClCompile/v:AdditionalOptions',ns)
            if opts is None:continue
            flags=opts.text or '';release='Release|x64' in group.attrib.get('Condition','')
            if not release:assert '-fprofile-' not in flags
            else:
                release_seen=True;assert '-flto=thin' in flags
                if mode=='baseline':assert '-fprofile-' not in flags
                if mode=='generate':assert '-fprofile-generate' in flags and '-fprofile-update=atomic' in flags and '-fprofile-use' not in flags
                if mode=='pgo':assert '-fprofile-use=' in flags and i['profile_sha256'] in flags and '-fprofile-generate' not in flags
        assert release_seen
for file,value in i['source_files'].items():assert sha(ROOT/file)==value
assert not list((BASE/'profiles').glob('verification-*.profraw')),'Optimized or baseline executable still writes instrumentation profiles'
profile=read(BASE/'profile-use-checks.json')
for mode,objects in profile.items():
    for name,data in objects.items():
        assert data['profile_summary']==(mode=='pgo')
        assert (data['function_entry_count_records']>0)==(mode=='pgo')
        assert sha(BASE/mode/'openttd_lib.dir/Release'/(name+'.obj'))==data['object_sha256']
assert profile['pgo']['openttd']['world_entry_count']>=4096
invalid=read(BASE/'profile-invalidation.json')
assert invalid['initial_profile_sha256']==i['profile_sha256']!=invalid['updated_profile_sha256']
assert invalid['auto_reconfigure'] and invalid['object_recompiled'] and invalid['executable_exit_code']==0
assert invalid['updated_object_mtime_ns']>invalid['initial_object_mtime_ns']
assert 'Passed all 775 test cases with 10745430 assertions.' in (BASE/'tests.log').read_text(encoding='utf-8',errors='replace')
assert not re.search(r'profile.*out.of.date|hash mismatch|mismatched data',(BASE/'build-pgo.log').read_text(encoding='utf-8',errors='replace'),re.I)
assert sha(BASE/'pgo/Release/openttd_test.exe')==i['test_binary_sha256']
reference=read(ROOT/'docs/measurements/compiler-lto-real-map-2026-10-19/summary.json')['canonical']['wpr-clang-1']
runs=[('training','generate',4096),('runs/plain-baseline-1','baseline',1024),('runs/plain-pgo-1','pgo',1024)]
if i['sampled_cpu_available']:runs += [(f'runs/wpr-{mode}-1',mode,1024) for mode in ('baseline','pgo')]
for name,mode,ticks in runs:
    r=read(BASE/name/'process.json');clock=r['actual-ticks.json']
    assert r['exit_code']==0 and r['warm_complete'] and r['simulation_timing_valid'] and not r['load_failed'] and r['save-integrity.json']['payments_valid']
    assert clock['start_tick']==3197276448 and clock['end_tick']==3197276448+ticks
    assert [s[0] for s in clock['samples']]==list(range(3197276449,3197276449+ticks))
    assert r['binary_sha256']==i['builds'][mode]['exe_sha256']
    if ticks==1024:assert r['save-integrity.json']==reference
for server,client in [('baseline','pgo'),('pgo','baseline')]:
    r=read(BASE/f'mp-{server}-{client}/result.json')
    assert r['pass'] and r['joined'] and not r['network_error'] and r['actual_ticks']>=256
    assert r['server_snapshot']==r['client_snapshot'] and r['transport_server']==r['transport_client']
    assert r['replay']['start_tick']==r['replay']['end_tick']==3197276448
    assert r['transport_server']['state_ticks']-r['server_snapshot']['state_ticks']==r['actual_ticks']
    assert r['server_binary_sha256']==i['builds'][server]['exe_sha256'] and r['client_binary_sha256']==i['builds'][client]['exe_sha256']
assert read(BASE/'mp-baseline-pgo/result.json')['server_snapshot']==read(BASE/'mp-pgo-baseline/result.json')['server_snapshot']
if i['sampled_cpu_available']:
    s=read(BASE/'summary.json');assert not any(s['mismatches'].values())
    for run in s['runs']:
        text=(BASE/'runs'/run['name']/'trace-stats.txt').read_text(encoding='utf-8-sig')
        assert 'Total # Lost Buffers : 0' in text and 'Total # Lost Events  : 0' in text
assert not any(read(BASE/'summary-plain.json')['mismatches'].values())
for p in (OUT/'tools').glob('*.py'):ast.parse(p.read_text(encoding='utf-8'))
report=(ROOT/'docs/clang-thin-pgo-2026-10-19.md').read_text(encoding='utf-8')
assert not re.search(r'<!-- (OUTCOME|PROFILE|RESULTS|MULTIPLAYER|ARTIFACTS) -->',report)
print('PASS: original input, IR profile consumption, profile snapshot, no instrumentation output, automatic rebuild, 775 tests, exact tick states, reference states and both multiplayer directions')
