"""Four counterbalanced rounds; each round is Legacy then Hyperpath from the frozen input."""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OLD=ROOT/'build-wpr-abba-6c11c67f-2026-10-19'
INPUT=ROOT/'build-wpr-clang-thin-2026-10-19/input'
BASE=ROOT/'docs/measurements/passenger-wpr-abba-6c11c67f-2026-10-19/02-B1'
TOOLKIT=ROOT/'build-wpr-9ab0228d/tools/wpt/Windows Kits/10/Windows Performance Toolkit'
sys.path.insert(0,str(ROOT/'utils'))
import passenger_precomputed_real_map as real
from passenger_wpr_window import audit_window

ROUNDS=[('01-A1','A'),('02-B1','B'),('03-B2','B'),('04-A2','A')]
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def dump(p,x):p.write_text(json.dumps(x,indent=2,ensure_ascii=False)+'\n',encoding='utf-8',newline='\n')
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def wait(p,seconds=2400):
    until=time.monotonic()+seconds
    while not p.is_file():
        if time.monotonic()>until:raise TimeoutError(str(p))
        if (WORK/'recorder.log').is_file() and list(WORK.rglob('wpr-error.txt')):raise RuntimeError('Recorder failed')
        time.sleep(.2)
def link(a,b):
    try:os.link(a,b)
    except OSError:shutil.copy2(a,b)
    return str(b)
def compare(a,b):
    assert len(a)==len(b)==24
    diff={k:[a.get(k),b.get(k)] for k in sorted(a.keys()|b.keys()) if a.get(k)!=b.get(k)}
    return dict(equal=not diff,fields=24,differences=diff)

def setup():
    wait(WORK/'build-ready.txt');wait(WORK/'symbols-ready.txt')
    inputs=load(WORK/'input-identity.json');package=load(INPUT/'package.json')
    assert sha(INPUT/'input.sav')==inputs['input_sha256']
    assert sha(INPUT/'openttd.cfg')==package['config_sha256']
    assert all(sha(INPUT/e['path'])==e['sha256'] for e in package['assets'])
    for name,label in ROUNDS:
        saved=OLD if label=='A' else WORK
        identity=load(saved/'build-identity.json')
        for fn in ['openttd.exe','openttd.pdb']:
            assert sha(saved/'Release'/fn)==identity['files']['Release/'+fn]['sha256']
        folder=WORK/name;folder.mkdir()
        (folder/'Release').mkdir()
        for fn in ['openttd.exe','openttd.pdb']:link(saved/'Release'/fn,folder/'Release'/fn)
        shutil.copytree(saved/'lang',folder/'lang',copy_function=link)
        shutil.copy2(WORK/'cpu-sampled.wpaProfile',folder/'cpu-sampled.wpaProfile')
        if (OLD/'02-B1/symcache').is_dir():shutil.copytree(OLD/'02-B1/symcache',folder/'symcache',copy_function=link)
    (WORK/'setup-ready.txt').write_text('Four immutable version-matched roots ready\n',encoding='utf-8',newline='\n')

def measure():
    wait(WORK/'setup-ready.txt');wait(WORK/'sampling-jobs/00-pilot/done.txt')
    assert load(WORK/'sampling-jobs/00-pilot/quality.json')['valid']
    inputs=load(WORK/'input-identity.json');results={};first={};counter=0
    try:
        for name,label in ROUNDS:
            saved=OLD if label=='A' else WORK
            identity=load(saved/'build-identity.json');folder=WORK/name
            for mode in ['legacy','hyperpath']:
                counter+=1;key=name+'/'+mode
                real.PROFILE.wait_for_idle(2400,60)
                run=folder/mode;assert not run.exists()
                assert sha(folder/'Release/openttd.exe')==identity['files']['Release/openttd.exe']['sha256']
                job=WORK/'sampling-jobs'/f'{counter:02}-{name}-{mode}';job.mkdir()
                command=[sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',str(folder/'Release/openttd.exe'),str(INPUT),str(run),
                         '--ticks','2048','--timeout','2400','--measurement-wait','2400','--resume','--precomputed',
                         '--hyperpath' if mode=='hyperpath' else '--no-hyperpath','--warm-precomputed','--wait-background-ready',
                         '--hyperpath-filter-mode','index','--main-thread-affinity-mask','1','--lang',str(folder/'lang'),'--wpr-profile','--wait-saved-replay']
                env={k:v for k,v in os.environ.items() if not k.startswith(('OPENTTD_','LLVM_PROFILE_'))}
                with (WORK/(name+'-'+mode+'-runner.log')).open('wb') as output:
                    process=subprocess.Popen(command,cwd=ROOT,env=env,stdout=output,stderr=subprocess.STDOUT)
                    until=time.monotonic()+30
                    while not run.is_dir():
                        if process.poll() is not None or time.monotonic()>until:raise RuntimeError('Runner failed before creating '+key)
                        time.sleep(.05)
                    temp=job/'job.tmp';dump(temp,dict(kind='map',directory=key,mode=mode));temp.replace(job/'job.json')
                    assert process.wait()==0,'Runner failed: '+key
                wait(job/'done.txt',60)
                quality=load(job/'quality.json');assert quality['valid'],quality
                meta=load(run/'process.json');ticks=meta['actual-ticks.json']
                assert meta['exit_code']==0 and meta['simulation_timing_valid'] and meta['warm_complete']
                assert meta['saved_replay']['ready'] and meta['background_ready']['completed']
                assert meta['background_ready']['tick_before']==meta['background_ready']['tick_after']==inputs['start_tick']
                assert ticks['start_tick']==inputs['start_tick'] and ticks['end_tick']==inputs['target_end_tick'] and len(ticks['samples'])==2048
                assert meta['input_sha256']==inputs['input_sha256'] and meta['binary_sha256']==identity['files']['Release/openttd.exe']['sha256']
                audit=audit_window(run,TOOLKIT,mode)
                actual=meta['save-integrity.json'];assert actual['payments_valid']
                canonical={'previous_6c11c67f':compare(load(BASE/mode/'save-integrity.json'),actual)}
                if mode in first:canonical['first_A_this_series']=compare(first[mode],actual)
                else:first[mode]=actual
                console=(run/'on-1-console.txt').read_text(encoding='utf-8-sig');diagnostics={}
                for category in ['memo','lookup']:
                    lines=re.findall(r'^\s*Passenger hyperpath '+category+r': (.+)$',console,re.MULTILINE);assert len(lines)==1
                    diagnostics[category]={k:int(v) for k,v in (f.split('=',1) for f in lines[0].strip().split('; '))}
                diagnostics['parallel_rows']=re.findall(r'^\s*Precomputed parallel rows: (.+)$',console,re.MULTILINE)
                results[key]=dict(round=name,label=label,mode=mode,product_commit=identity['product_commit'],command=command,quality=quality,window_audit=audit,
                    canonical=canonical,diagnostics=diagnostics,exact_ticks=2048,input_sha256=meta['input_sha256'],binary_sha256=meta['binary_sha256'],
                    disk_free_bytes=shutil.disk_usage(WORK).free)
                dump(WORK/'capture-results.json',results)
                print(key+': exact ticks, zero losses, valid window, canonical='+str(all(x['equal'] for x in canonical.values())),flush=True)
                assert all(x['equal'] for x in canonical.values()),'Canonical mismatch retained; stop queue'
                assert results[key]['disk_free_bytes']>2*1024**3,'Insufficient disk space'
        (WORK/'captures-complete.txt').write_text('All eight ABBA captures accepted\n',encoding='utf-8',newline='\n')
    finally:(WORK/'recorder-stop.txt').write_text('Controller ended\n',encoding='utf-8',newline='\n')

if __name__=='__main__':{'setup':setup,'measure':measure}[sys.argv[1]]()
