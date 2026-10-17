from pathlib import Path
import sys,os,json,tempfile,subprocess,re

root=Path('C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches')
build=root/'build-wpr-8462e6cd'
sys.path.insert(0,str(root/'utils'))
import passenger_precomputed_real_map as real
lease=Path(tempfile.gettempdir())/'openttd-passenger-routing-measurement.lock'
lock=real.PROFILE.acquire_measurement_lock(lease,1800)
try:
    os.write(lock,json.dumps(dict(pid=os.getpid(),purpose='8462e6cd three-stage native preflight',output=str(build))).encode())
    real.PROFILE.wait_for_idle(120,5)
    assert (build/'Release/openttd.pdb').is_file()
    with (build/'native-all.log').open('wb') as out:
        subprocess.run([str(build/'Release/openttd_test.exe'),'--reporter','compact'],stdout=out,stderr=subprocess.STDOUT,check=True)
    with (build/'pdb-rsds.txt').open('wb') as out:
        subprocess.run(['C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/dumpbin.exe','/headers',str(build/'Release/openttd.exe')],stdout=out,stderr=subprocess.STDOUT,check=True)
    print((build/'native-all.log').read_text(encoding='utf-8',errors='replace')[-200:],flush=True)
finally:
    os.close(lock)
    lease.unlink()
env=dict(os.environ)
for name in ['OPENTTD_PASSENGER_TRACE','OPENTTD_PASSENGER_CHECKPOINT_TICK','OPENTTD_PASSENGER_FOCUS_TRACE','OPENTTD_PASSENGER_DEPARTURE_DIAGNOSTIC','OPENTTD_RETIREMENT_PROFILE','OPENTTD_RETIREMENT_REFERENCE']:
    env.pop(name,None)
for name in ['validate','alternate','legacy','hyperpath']:
    print('Beginning '+name,flush=True)
    command=[sys.executable,'-B',str(root/'utils/passenger_precomputed_real_map.py'),'run',str(build/'Release/openttd.exe'),'C:/Users/bebe/.codex/worktrees/5a69/OpenTTD-patches/build-hyperpath-cpu/frozen',str(build/name),'--ticks','2048','--timeout','1800','--measurement-wait','120','--precomputed','--no-hyperpath' if name=='legacy' else '--hyperpath','--cold-precomputed','--warm-precomputed','--wait-background-ready','--main-thread-affinity-mask','1','--lang',str(build/'lang')]
    if name=='alternate':
        command+=['--hyperpath-filter-alternate']
    else:
        command+=['--hyperpath-filter-mode','validate' if name=='validate' else 'index']
    if name in ['legacy','hyperpath']:
        command+=['--wpr-profile']
    with (build/(name+'-runner.log')).open('wb') as out:
        subprocess.run(command,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
    meta=json.loads((build/name/'process.json').read_text(encoding='utf-8'))
    assert meta['exit_code']==0 and meta['simulation_timing_valid']
    assert not meta['contamination'] and not meta['phase_contamination']
    reference=root/'docs/measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17'/('legacy' if name=='legacy' else 'hyperpath')/'save-integrity.json'
    state=json.loads((build/name/'save-integrity.json').read_text(encoding='utf-8'))
    assert state==json.loads(reference.read_text(encoding='utf-8')),name+' canonical mismatch'
    console=(build/name/'on-1-console.txt').read_text(encoding='utf-8',errors='replace')
    lookup=re.findall(r'Passenger hyperpath lookup: rebuilds=(\d+); indexed_groups=(\d+); validated_groups=(\d+); violations=(\d+)',console)
    assert lookup and int(lookup[-1][3])==0
    if name=='validate': assert int(lookup[-1][2])>0
    print('Completed '+name+'; canonical 24/24; lookup '+str(lookup[-1]),flush=True)
print('All four 2048-tick replays completed',flush=True)
