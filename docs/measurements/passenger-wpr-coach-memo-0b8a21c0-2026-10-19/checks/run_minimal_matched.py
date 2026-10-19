"""After the zero-loss pilot, capture old/new x Legacy/Hyperpath with one identical profile."""
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
sys.path.insert(0,str(ROOT/'utils'))
import passenger_precomputed_real_map as real
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def link_or_copy(source,dest):
    try:os.link(source,dest)
    except OSError:shutil.copy2(source,dest)
    return str(dest)
deadline=time.monotonic()+3600
while not (WORK/'sampling-jobs/pilot/done.txt').is_file():
    if time.monotonic()>deadline:raise TimeoutError('Minimal profile pilot did not finish')
    time.sleep(1)
assert load(WORK/'sampling-jobs/pilot/quality.json')['valid']
for mode,rejected_name in [('legacy','rejected-legacy-buffered-events'),('hyperpath','rejected-hyperpath-sampled-events')]:
    source=WORK/mode;dest=WORK/rejected_name
    assert source.resolve().parent==dest.resolve().parent==WORK
    if source.exists():
        assert not dest.exists() and not source.is_symlink() and not source.is_junction()
        source.rename(dest)
    else:assert dest.exists()
CONTROL.mkdir(exist_ok=True);(CONTROL/'Release').mkdir(exist_ok=True)
identity=load(HIST/'build-identity.json')
for name in ['openttd.exe','openttd.pdb']:
    assert sha(OLD/'Release'/name)==identity['files']['Release/'+name]['sha256']
    shutil.copy2(OLD/'Release'/name,CONTROL/'Release'/name)
for folder in ['lang','symcache']:
    if not (CONTROL/folder).exists():shutil.copytree(OLD/folder,CONTROL/folder,copy_function=link_or_copy)
shutil.copy2(WORK/'cpu-sampled.wpaProfile',CONTROL/'cpu-sampled.wpaProfile')
(CONTROL/'build-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
results={};inputs=load(WORK/'input-identity.json')
for index,(role,root,mode,replay) in enumerate([('control',CONTROL,'legacy',False),('control',CONTROL,'hyperpath',False),('target',WORK,'legacy',True),('target',WORK,'hyperpath',True)],1):
    label=f'{index:02}-{role}-{mode}'
    real.PROFILE.wait_for_idle(2400,60)
    job=WORK/'sampling-jobs'/label;job.mkdir(exist_ok=True)
    command=[sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',str(root/'Release/openttd.exe'),str(WORK/'input'),str(root/mode),
        '--ticks','2048','--timeout','2400','--measurement-wait','2400','--resume','--precomputed','--hyperpath' if mode=='hyperpath' else '--no-hyperpath',
        '--warm-precomputed','--wait-background-ready','--hyperpath-filter-mode','index','--main-thread-affinity-mask','1','--lang',str(root/'lang'),'--wpr-profile']
    if replay:command.append('--wait-saved-replay')
    env={k:v for k,v in os.environ.items() if not k.startswith('OPENTTD_')}
    with (WORK/(label+'-runner.log')).open('wb') as out:
        process=subprocess.Popen(command,cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT)
        while not (root/mode).is_dir():
            if process.poll() is not None:raise RuntimeError('Runner failed before creating its output directory')
            time.sleep(.05)
        (job/'job.json').write_text(json.dumps(dict(kind='map',directory=(root/mode).relative_to(WORK).as_posix(),mode=mode),indent=2)+'\n')
        assert process.wait()==0,'Map runner failed'
    meta=load(root/mode/'process.json');ticks=meta['actual-ticks.json']
    assert meta['exit_code']==0 and meta['simulation_timing_valid'] and meta['warm_complete'] and meta['background_ready']['completed']
    assert meta['background_ready']['tick_before']==meta['background_ready']['tick_after']==inputs['start_tick']
    assert ticks['start_tick']==inputs['start_tick'] and ticks['end_tick']==inputs['target_end_tick'] and len(ticks['samples'])==2048
    before=load(HIST/mode/'save-integrity.json');after=meta['save-integrity.json'];assert len(before)==len(after)==24
    diff={k:[before.get(k),after.get(k)] for k in sorted(before.keys()|after.keys()) if before.get(k)!=after.get(k)}
    quality=load(job/'quality.json');assert quality['valid'],quality
    results[label]=dict(role=role,mode=mode,command=command,canonical=dict(equal=not diff,fields=24,differences=diff),
        input_sha256=meta['input_sha256'],binary_sha256=meta['binary_sha256'],quality=quality,profile_sha256=sha(WORK/'cpu-minimal.wprp'))
    (WORK/'final-controls.json').write_text(json.dumps(results,indent=2)+'\n')
    if role=='target':
        canonical=load(WORK/'canonical-comparison.json');canonical[mode]=results[label]['canonical']
        (WORK/'canonical-comparison.json').write_text(json.dumps(canonical,indent=2)+'\n')
    print(label+': zero loss, both markers, canonical '+str(not diff),flush=True)
(WORK/'final-captures-complete.txt').write_text('Four matched-profile traces completed\n')
(WORK/'wpr-final-controls-complete.txt').write_text('All four recorder jobs passed quality checks\n')
(WORK/'minimal-recorder-stop.txt').write_text('All requested matched WPR jobs finished\n')
