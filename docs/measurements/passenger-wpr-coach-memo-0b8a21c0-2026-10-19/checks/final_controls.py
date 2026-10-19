"""Record matched CPU-only controls at 97cdc0ad and the target Legacy run."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OLD=ROOT/'build-wpr-97cdc0ad'
HIST=ROOT/'docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
CONTROL=WORK/'control-97cdc0ad'
TOOLKIT=ROOT/'build-wpr-9ab0228d/tools/wpt/Windows Kits/10/Windows Performance Toolkit'
sys.path.insert(0,str(ROOT/'utils'))
import passenger_precomputed_real_map as real
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def link_or_copy(source,dest):
    try:os.link(source,dest)
    except OSError:shutil.copy2(source,dest)
    return str(dest)
deadline=time.monotonic()+7200
while not ((WORK/'hyperpath-buffered-complete.txt').is_file() and (WORK/'wpr-buffered-recorder-complete.txt').is_file()):
    if time.monotonic()>deadline:raise TimeoutError('CPU-only Hyperpath did not complete')
    time.sleep(2)
# Fail early before more runs if the proven CPU-only profile still loses data.
with (WORK/'hyperpath-sampled-export.log').open('wb') as out:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_export.py'),str(WORK),str(TOOLKIT),
        '--mode','hyperpath','--output-name','wpa','--local-symbols-only'],stdout=out,stderr=subprocess.STDOUT,check=True)
CONTROL.mkdir(exist_ok=True);(CONTROL/'Release').mkdir(exist_ok=True)
identity=load(HIST/'build-identity.json')
for name in ['openttd.exe','openttd.pdb']:
    assert sha(OLD/'Release'/name)==identity['files']['Release/'+name]['sha256']
    shutil.copy2(OLD/'Release'/name,CONTROL/'Release'/name)
for folder in ['lang','symcache']:shutil.copytree(OLD/folder,CONTROL/folder,copy_function=link_or_copy)
shutil.copy2(WORK/'cpu-sampled.wpaProfile',CONTROL/'cpu-sampled.wpaProfile')
(CONTROL/'build-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
source=WORK/'legacy';destination=WORK/'rejected-legacy-buffered-events'
assert source.resolve().parent==destination.resolve().parent==WORK and not destination.exists()
assert not source.is_symlink() and not source.is_junction()
assert load(source/'save-integrity.json')==load(HIST/'legacy/save-integrity.json')
source.rename(destination)
(WORK/'final-captures-stage-start.txt').write_text('Earlier recorder complete; matched CPU-only runs may start\n')
results={}
for label,root,mode,replay in [('target-legacy',WORK,'legacy',True),('control-legacy',CONTROL,'legacy',False),('control-hyperpath',CONTROL,'hyperpath',False)]:
    real.PROFILE.wait_for_idle(2400,60)
    (WORK/(label+'-stage-ready.txt')).write_text('Fresh run may be recorded\n')
    command=[sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',str(root/'Release/openttd.exe'),str(WORK/'input'),str(root/mode),
        '--ticks','2048','--timeout','2400','--measurement-wait','2400','--resume','--precomputed','--hyperpath' if mode=='hyperpath' else '--no-hyperpath',
        '--warm-precomputed','--wait-background-ready','--hyperpath-filter-mode','index','--main-thread-affinity-mask','1','--lang',str(root/'lang'),'--wpr-profile']
    if replay:command.append('--wait-saved-replay')
    env={k:v for k,v in os.environ.items() if not k.startswith('OPENTTD_')}
    with (WORK/(label+'-runner.log')).open('wb') as out:subprocess.run(command,cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
    meta=load(root/mode/'process.json');ticks=meta['actual-ticks.json'];inputs=load(WORK/'input-identity.json')
    assert meta['exit_code']==0 and meta['simulation_timing_valid'] and meta['warm_complete']
    assert meta['background_ready']['completed'] and meta['background_ready']['tick_before']==meta['background_ready']['tick_after']==inputs['start_tick']
    assert ticks['start_tick']==inputs['start_tick'] and ticks['end_tick']==inputs['target_end_tick'] and len(ticks['samples'])==2048
    before=load(HIST/mode/'save-integrity.json');after=meta['save-integrity.json'];assert len(before)==len(after)==24
    differences={k:[before.get(k),after.get(k)] for k in sorted(before.keys()|after.keys()) if before.get(k)!=after.get(k)}
    results[label]=dict(command=command,exact_ticks=2048,canonical=dict(equal=not differences,fields=24,differences=differences),
        input_sha256=meta['input_sha256'],binary_sha256=meta['binary_sha256'],cpu_profile='CPU-only File, system=1024 event=128 buffers, 1ms')
    (WORK/'final-controls.json').write_text(json.dumps(results,indent=2)+'\n')
    if label=='target-legacy':
        canonical=load(WORK/'canonical-comparison.json');canonical['legacy']=results[label]['canonical']
        (WORK/'canonical-comparison.json').write_text(json.dumps(canonical,indent=2)+'\n')
    print(label+': canonical '+str(not differences),flush=True)
(WORK/'final-captures-complete.txt').write_text('Target pair and matched 97cdc0ad pair complete\n')
