"""After buffered Legacy, replace the dropped-events Hyperpath trace with a buffered retake."""
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
while not (WORK/'legacy-file-complete.txt').is_file():
    if time.monotonic()>deadline:raise TimeoutError('Buffered Legacy did not complete')
    time.sleep(2)
source=WORK/'hyperpath';destination=WORK/'rejected-hyperpath-dropped-events'
assert source.resolve().parent==destination.resolve().parent==WORK and not destination.exists()
assert not source.is_symlink() and not source.is_junction()
assert load(source/'save-integrity.json')==load(BASE/'hyperpath/save-integrity.json')
assert 'dropped 2024287 events' in (WORK/'wpr-recorder.log').read_text(encoding='utf-8-sig')
source.rename(destination)
shutil.copy2(WORK/'hyperpath-runner.log',WORK/'rejected-hyperpath-runner.log')
(WORK/'hyperpath-buffered-stage-ready.txt').write_text('Dropped-events trace isolated; buffered retake may start\n')
real.PROFILE.wait_for_idle(2400,60)
command=[sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',str(WORK/'Release/openttd.exe'),str(WORK/'input'),str(WORK/'hyperpath'),
    '--ticks','2048','--timeout','2400','--measurement-wait','2400','--resume','--wait-saved-replay','--precomputed','--hyperpath','--warm-precomputed',
    '--wait-background-ready','--hyperpath-filter-mode','index','--main-thread-affinity-mask','1','--lang',str(WORK/'lang'),'--wpr-profile']
env={k:v for k,v in os.environ.items() if not k.startswith('OPENTTD_')}
with (WORK/'hyperpath-buffered-runner.log').open('wb') as out:subprocess.run(command,cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
meta=load(WORK/'hyperpath/process.json');ticks=meta['actual-ticks.json'];inputs=load(WORK/'input-identity.json')
assert meta['exit_code']==0 and meta['simulation_timing_valid'] and meta['warm_complete'] and meta['saved_replay']['ready']
assert ticks['start_tick']==inputs['start_tick'] and ticks['end_tick']==inputs['target_end_tick'] and len(ticks['samples'])==2048
old=load(BASE/'hyperpath/save-integrity.json');actual=meta['save-integrity.json'];assert len(old)==len(actual)==24
differences={k:[old.get(k),actual.get(k)] for k in sorted(old.keys()|actual.keys()) if old.get(k)!=actual.get(k)}
canonical=load(WORK/'canonical-comparison.json');canonical['hyperpath']=dict(equal=not differences,fields=24,differences=differences)
(WORK/'canonical-comparison.json').write_text(json.dumps(canonical,indent=2)+'\n')
(WORK/'hyperpath-buffered-complete.txt').write_text('Buffered capture complete; canonical compared\n')
print(json.dumps(canonical,indent=2),flush=True)
