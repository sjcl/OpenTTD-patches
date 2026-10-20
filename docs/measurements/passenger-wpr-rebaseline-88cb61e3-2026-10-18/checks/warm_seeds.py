"""Create independent steady seeds with 8462e6cd from the replacement input."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
BUILD=WORK/'baseline'
RUNNER=ROOT/'utils/passenger_precomputed_real_map.py'
ORIGINAL=WORK/'original'

def load(p): return json.loads(p.read_text(encoding='utf-8'))
def dump(p,d): p.write_text(json.dumps(d,indent=2)+'\n',encoding='utf-8')
def sha(p):
    with p.open('rb') as f: return hashlib.file_digest(f,'sha256').hexdigest()

env=dict(os.environ)
for k in ['OPENTTD_PASSENGER_TRACE','OPENTTD_PASSENGER_CHECKPOINT_TICK','OPENTTD_PASSENGER_FOCUS_TRACE',
          'OPENTTD_PASSENGER_DEPARTURE_DIAGNOSTIC','OPENTTD_RETIREMENT_PROFILE','OPENTTD_RETIREMENT_REFERENCE']:
    env.pop(k,None)
deadline=time.monotonic()+1800
while not (BUILD/'native-all.log').is_file() or 'Passed all 728 test cases' not in (BUILD/'native-all.log').read_text(errors='replace'):
    if time.monotonic()>deadline: raise TimeoutError('Baseline native build/tests did not succeed')
    time.sleep(2)
assert sha(ORIGINAL/'input.sav')==load(ORIGINAL/'package.json')['input_sha256']=='79fa148791f6abbf76f9b64852b475d2e937383d949413c4ffe55057f76c51c3'
seeds=WORK/'seeds';seeds.mkdir(exist_ok=True)
result={}
for mode in ['legacy','hyperpath']:
    output=WORK/('warm-'+mode)
    print('Beginning 8192-tick '+mode+' warmup',flush=True)
    command=[sys.executable,'-B',str(RUNNER),'run',str(BUILD/'Release/openttd.exe'),str(ORIGINAL),str(output),
        '--ticks','8192','--timeout','3600','--measurement-wait','2400','--validation-only','--precomputed',
        '--no-hyperpath' if mode=='legacy' else '--hyperpath','--cold-precomputed','--warm-precomputed',
        '--wait-background-ready','--hyperpath-filter-mode','index','--main-thread-affinity-mask','1','--lang',str(BUILD/'lang')]
    run_env=dict(env,OPENTTD_PASSENGER_CHECKPOINT_TICK='4096')
    if not (output/'process.json').is_file():
        with (WORK/('warm-'+mode+'-runner.log')).open('wb') as f:
            subprocess.run(command,cwd=ROOT,env=run_env,stdout=f,stderr=subprocess.STDOUT,check=True)
    meta=load(output/'process.json');state=load(output/'save-integrity.json'); clock=meta['actual-ticks.json']
    # Seed creation/reload checks are correctness work, never performance evidence.
    # Preserve the completed Legacy warmup's raw invalid timing flags and observations.
    assert meta['exit_code']==0 and not meta['load_failed'] and meta['warm_complete'] and state['payments_valid']
    assert clock['end_tick']-clock['start_tick']==len(clock['samples'])==8192
    assert state['state_ticks']==clock['end_tick'] and len(state)==24
    package=seeds/mode
    subprocess.run([sys.executable,'-B',str(RUNNER),'freeze',str(output/'result.sav'),str(ORIGINAL),str(BUILD),str(package)],cwd=ROOT,check=True)
    assert sha(package/'input.sav')==meta['result_sha256']
    reload=WORK/('reload-'+mode)
    with (WORK/('reload-'+mode+'-runner.log')).open('wb') as f:
        subprocess.run([sys.executable,'-B',str(RUNNER),'run',str(BUILD/'Release/openttd.exe'),str(package),str(reload),
            '--ticks','0','--timeout','1800','--measurement-wait','2400','--validation-only','--resume','--main-thread-affinity-mask','1',
            '--lang',str(BUILD/'lang')],cwd=ROOT,env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
    loaded=load(reload/'save-integrity.json')
    diff={k:dict(saved=state.get(k),reloaded=loaded.get(k)) for k in state.keys()|loaded.keys() if state.get(k)!=loaded.get(k)}
    result[mode]=dict(input_sha256=sha(package/'input.sav'),start_tick=clock['start_tick'],frozen_tick=state['state_ticks'],
        canonical=state,reload_equal=not diff,reload_differences=diff,fields=24,
        warmup_used_for_timing=False,warmup_raw_simulation_timing_valid=meta['simulation_timing_valid'],
        warmup_contamination_observations=len(meta['contamination']))
    dump(WORK/'seed-validation.json',result)
    assert not diff,diff
    (package/'input.sav').chmod(stat.S_IREAD)
    print(mode+' frozen; reload canonical 24/24 identical',flush=True)
dump(WORK/'seeds-complete.json',dict(complete=True,seeds=result))
