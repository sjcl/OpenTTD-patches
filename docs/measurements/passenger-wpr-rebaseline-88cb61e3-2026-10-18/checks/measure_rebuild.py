"""Interleave old/new WPR captures from exactly the same per-mode steady seed."""
from pathlib import Path
import json
import os
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
PARENT=Path(__file__).resolve().parent
WORK=PARENT/'rebuild'
sys.path.insert(0,str(ROOT/'utils'))
import passenger_precomputed_real_map as real

def load(p): return json.loads(p.read_text(encoding='utf-8'))
env=dict(os.environ)
for k in ['OPENTTD_PASSENGER_TRACE','OPENTTD_PASSENGER_CHECKPOINT_TICK','OPENTTD_PASSENGER_FOCUS_TRACE',
          'OPENTTD_PASSENGER_DEPARTURE_DIAGNOSTIC','OPENTTD_RETIREMENT_PROFILE','OPENTTD_RETIREMENT_REFERENCE']:
    env.pop(k,None)
seeds=load(PARENT/'seeds-complete.json')['seeds']; checks={}
def wait_clean_host():
    deadline=time.monotonic()+2400
    previous=None
    while True:
        real.PROFILE.wait_for_idle(max(0,deadline-time.monotonic()),60)
        nodes=real.PROFILE.powershell("Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'MSBuild.exe' -and $_.CommandLine -match '/nodemode:' } | ForEach-Object { $_.ProcessId }").strip()
        if not nodes: return
        if nodes!=previous:
            print('Waiting for remaining MSBuild node lifetimes to end: '+nodes.replace('\n',', '),flush=True); previous=nodes
        if time.monotonic()>deadline: raise TimeoutError('MSBuild nodes remain; strict kernel-lifetime audit would reject the WPR window')
        time.sleep(10)
for mode in ['legacy','hyperpath']:
    for version in ['baseline','candidate']:
        build=WORK/version; output=build/mode
        wait_clean_host()
        print('Beginning '+version+' '+mode+' WPR',flush=True)
        with (build/(mode+'-runner.log')).open('wb') as f:
            subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',
                str(build/'Release/openttd.exe'),str(PARENT/'seeds'/mode),str(output),
                '--ticks','2048','--timeout','2400','--measurement-wait','2400','--resume','--precomputed',
                '--no-hyperpath' if mode=='legacy' else '--hyperpath','--warm-precomputed','--wait-background-ready',
                '--hyperpath-filter-mode','index','--main-thread-affinity-mask','1','--lang',str(build/'lang'),'--wpr-profile','--rebuild-precomputed'],
                cwd=ROOT,env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
        meta=load(output/'process.json');state=load(output/'save-integrity.json');clock=meta['actual-ticks.json']
        assert meta['exit_code']==0 and meta['simulation_timing_valid'] and state['payments_valid']
        assert clock['end_tick']-clock['start_tick']==len(clock['samples'])==2048
        assert clock['start_tick']==seeds[mode]['frozen_tick'] and meta['input_sha256']==seeds[mode]['input_sha256']
        assert len(state)==24
        assert meta['rebuild_precomputed'] is True
        assert state['table_operations'] != seeds[mode]['canonical']['table_operations'], 'No replacement build work was observed'
        if version=='candidate':
            old=load(WORK/'baseline'/mode/'save-integrity.json')
            diff={k:dict(old=old.get(k),new=state.get(k)) for k in old.keys()|state.keys() if old.get(k)!=state.get(k)}
            checks[mode]=dict(equal=not diff,fields=24,differences=diff,input_sha256=meta['input_sha256'])
            (WORK/'canonical-comparison.json').write_text(json.dumps(checks,indent=2)+'\n')
            assert not diff,diff
        print('Completed '+version+' '+mode+'; 2048 ticks'+('; canonical 24/24' if version=='candidate' else ''),flush=True)
print('All four WPR replays completed',flush=True)
