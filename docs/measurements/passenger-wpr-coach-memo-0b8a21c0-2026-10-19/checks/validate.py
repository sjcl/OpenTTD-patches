"""After WPR finishes, validate coach memo reuse for another 2048 ticks from the same input."""
from pathlib import Path
import json
import os
import re
import subprocess
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
BASE=ROOT/'docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
sys.path.insert(0,str(ROOT/'utils'))
import passenger_precomputed_real_map as real
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
deadline=time.monotonic()+7200
while not ((WORK/'final-captures-complete.txt').is_file() and (WORK/'wpr-final-controls-complete.txt').is_file()):
    if time.monotonic()>deadline:raise TimeoutError('WPR captures did not complete')
    time.sleep(2)
real.PROFILE.wait_for_idle(2400,60)
command=[sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',str(WORK/'Release/openttd.exe'),str(WORK/'input'),str(WORK/'validate'),
    '--ticks','2048','--timeout','2400','--measurement-wait','2400','--resume','--wait-saved-replay','--precomputed','--hyperpath','--warm-precomputed','--wait-background-ready',
    '--hyperpath-filter-mode','validate','--main-thread-affinity-mask','1','--lang',str(WORK/'lang')]
env={k:v for k,v in os.environ.items() if not k.startswith('OPENTTD_')}
with (WORK/'validate-runner.log').open('wb') as out:
    subprocess.run(command,cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
meta=load(WORK/'validate/process.json');inputs=load(WORK/'input-identity.json');ticks=meta['actual-ticks.json']
assert meta['exit_code']==0 and meta['simulation_timing_valid'] and meta['hyperpath_filter_mode']=='validate'
assert ticks['start_tick']==inputs['start_tick'] and ticks['end_tick']==inputs['target_end_tick'] and len(ticks['samples'])==2048
console=(WORK/'validate/on-1-console.txt').read_text(encoding='utf-8-sig')
memo=re.findall(r'Passenger hyperpath memo: hits=(\d+); validated=(\d+); violations=(\d+)',console)
lookup=re.findall(r'Passenger hyperpath lookup: (.*)',console)
assert len(memo)==1 and len(lookup)==1,(memo,lookup)
hits,validated,violations=map(int,memo[0])
old=load(BASE/'hyperpath/save-integrity.json');actual=meta['save-integrity.json']
assert len(old)==len(actual)==24 and actual['payments_valid']
differences={k:[old.get(k),actual.get(k)] for k in sorted(old.keys()|actual.keys()) if old.get(k)!=actual.get(k)}
result=dict(command=command,exact_ticks=2048,simulation_timing_valid=True,input_sha256=meta['input_sha256'],hyperpath_filter_mode='validate',
    memo=dict(hits=hits,validated=validated,violations=violations),lookup_line=lookup[0],reuse_observed=validated>0,
    canonical=dict(equal=not differences,fields=24,differences=differences),excluded_from_performance_comparison=True)
(WORK/'validate-result.json').write_text(json.dumps(result,indent=2)+'\n')
(WORK/'validate-complete.txt').write_text('Validate run completed; result saved\n')
print(json.dumps(result,indent=2),flush=True)
