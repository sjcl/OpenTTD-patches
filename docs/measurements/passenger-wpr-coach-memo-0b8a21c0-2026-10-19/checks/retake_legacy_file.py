"""After Hyperpath, retake Legacy using file mode because memory mode lost its begin mark."""
from pathlib import Path
import json
import os
import shutil
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
while True:
    if (WORK/'wpr-recorder-complete.txt').is_file() and (WORK/'runs-complete.json').is_file():
        if set(load(WORK/'runs-complete.json'))=={'legacy','hyperpath'}:break
    if time.monotonic()>deadline:raise TimeoutError('Original pair did not finish')
    time.sleep(2)
source=WORK/'legacy';destination=WORK/'rejected-legacy-memory-markers'
assert source.resolve().parent==destination.resolve().parent==WORK and not destination.exists()
assert not source.is_symlink() and not source.is_junction()
marks=(source/'window-markers.txt').read_text(encoding='utf-8-sig')
assert 'legacy-end' in marks and 'legacy-begin' not in marks
source.rename(destination)
shutil.copy2(WORK/'legacy-runner.log',WORK/'rejected-legacy-memory-runner.log')
shutil.copy2(WORK/'legacy-window-runner.log',WORK/'rejected-legacy-window-runner.log')
(WORK/'legacy-file-stage-ready.txt').write_text('Memory capture isolated; prior recorder complete\n')
real.PROFILE.wait_for_idle(2400,60)
command=[sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',str(WORK/'Release/openttd.exe'),str(WORK/'input'),str(WORK/'legacy'),
    '--ticks','2048','--timeout','2400','--measurement-wait','2400','--resume','--wait-saved-replay','--precomputed','--no-hyperpath','--warm-precomputed',
    '--wait-background-ready','--hyperpath-filter-mode','index','--main-thread-affinity-mask','1','--lang',str(WORK/'lang'),'--wpr-profile']
env={k:v for k,v in os.environ.items() if not k.startswith('OPENTTD_')}
with (WORK/'legacy-file-runner.log').open('wb') as out:subprocess.run(command,cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
meta=load(WORK/'legacy/process.json');ticks=meta['actual-ticks.json'];inputs=load(WORK/'input-identity.json')
assert meta['exit_code']==0 and meta['simulation_timing_valid'] and meta['warm_complete'] and meta['saved_replay']['ready']
assert ticks['start_tick']==inputs['start_tick'] and ticks['end_tick']==inputs['target_end_tick'] and len(ticks['samples'])==2048
old=load(BASE/'legacy/save-integrity.json');actual=meta['save-integrity.json'];assert len(old)==len(actual)==24
differences={k:[old.get(k),actual.get(k)] for k in sorted(old.keys()|actual.keys()) if old.get(k)!=actual.get(k)}
canonical=load(WORK/'canonical-comparison.json');canonical['legacy']=dict(equal=not differences,fields=24,differences=differences)
(WORK/'canonical-comparison.json').write_text(json.dumps(canonical,indent=2)+'\n')
(WORK/'legacy-file-complete.txt').write_text('File capture complete; canonical compared\n')
print(json.dumps(canonical,indent=2),flush=True)
