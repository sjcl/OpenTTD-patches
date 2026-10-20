"""One pilot, then Legacy and Hyperpath 2048-tick captures in fresh processes."""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
BASE=ROOT/'docs/measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19'
HIST=ROOT/'docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
SIX=ROOT/'docs/measurements/passenger-hyperpath-validate-6aeaff68-2026-10-19'
TOOLKIT=ROOT/'build-wpr-9ab0228d/tools/wpt/Windows Kits/10/Windows Performance Toolkit'
sys.path.insert(0,str(ROOT/'utils'))
import passenger_precomputed_real_map as real
from passenger_wpr_window import audit_window

def load(path):return json.loads(path.read_text(encoding='utf-8-sig'))
def dump(path,value):path.write_text(json.dumps(value,indent=2,ensure_ascii=False)+'\n',encoding='utf-8',newline='\n')
def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def wait(path,seconds=2400):
    until=time.monotonic()+seconds
    while not path.is_file():
        if time.monotonic()>until:raise TimeoutError(str(path))
        time.sleep(.2)
def compare(old,new):
    assert len(old)==len(new)==24
    diff={k:[old.get(k),new.get(k)] for k in sorted(old.keys()|new.keys()) if old.get(k)!=new.get(k)}
    return dict(equal=not diff,fields=24,differences=diff)

wait(WORK/'build-ready.txt');wait(WORK/'sampling-jobs/00-pilot/done.txt')
assert load(WORK/'sampling-jobs/00-pilot/quality.json')['valid']
identity=load(WORK/'build-identity.json');inputs=load(WORK/'input-identity.json')
assert identity['product_commit']=='8f829b8c3d5f35cb420c2a16812716ef24e38939'
assert sha(WORK/'Release/openttd.exe')==identity['files']['Release/openttd.exe']['sha256']
assert sha(WORK/'cpu-minimal.wprp')=='e0a89dab28d5379469aa3ad23f65e8c87256014951065dae14b9429998251a82'
results={}
try:
    for index,mode in enumerate(['legacy','hyperpath'],1):
        real.PROFILE.wait_for_idle(2400,60)
        run=WORK/mode;assert not run.exists()
        job=WORK/'sampling-jobs'/f'{index:02}-{mode}';job.mkdir()
        command=[sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',str(WORK/'Release/openttd.exe'),str(WORK/'input'),str(run),
                 '--ticks','2048','--timeout','2400','--measurement-wait','2400','--resume','--precomputed',
                 '--hyperpath' if mode=='hyperpath' else '--no-hyperpath','--warm-precomputed','--wait-background-ready',
                 '--hyperpath-filter-mode','index','--main-thread-affinity-mask','1','--lang',str(WORK/'lang'),'--wpr-profile','--wait-saved-replay']
        env={k:v for k,v in os.environ.items() if not k.startswith('OPENTTD_')}
        with (WORK/(mode+'-runner.log')).open('wb') as output:
            process=subprocess.Popen(command,cwd=ROOT,env=env,stdout=output,stderr=subprocess.STDOUT)
            while not run.is_dir():
                if process.poll() is not None:raise RuntimeError('Runner failed before making its fresh directory')
                time.sleep(.05)
            temp=job/'job.tmp';dump(temp,dict(kind='map',directory=mode,mode=mode));temp.replace(job/'job.json')
            assert process.wait()==0,'Runner failed; inspect '+str(run)
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
        canonical={'97cdc0ad':compare(load(HIST/mode/'save-integrity.json'),actual),'0b8a21c0':compare(load(BASE/mode/'save-integrity.json'),actual)}
        if mode=='hyperpath':canonical['6aeaff68_validate']=compare(load(SIX/'validate/save-integrity.json'),actual)
        console=(run/'on-1-console.txt').read_text(encoding='utf-8-sig')
        diagnostics={}
        for category in ['memo','lookup']:
            lines=re.findall(r'^\s*Passenger hyperpath '+category+r': (.+)$',console,re.MULTILINE);assert len(lines)==1
            diagnostics[category]={k:int(v) for k,v in (field.split('=',1) for field in lines[0].strip().split('; '))}
        results[mode]=dict(command=command,quality=quality,window_audit=audit,canonical=canonical,diagnostics=diagnostics,
                           exact_ticks=2048,input_sha256=meta['input_sha256'],binary_sha256=meta['binary_sha256'])
        dump(WORK/'capture-results.json',results)
        print(mode+': exact ticks, zero loss, valid markers/window; canonical '+str(all(v['equal'] for v in canonical.values())),flush=True)
        assert all(v['equal'] for v in canonical.values()),'Canonical mismatch retained; stop before the next run'
    (WORK/'captures-complete.txt').write_text('Both requested WPR captures passed quality and canonical checks\n',encoding='utf-8',newline='\n')
finally:
    (WORK/'recorder-stop.txt').write_text('Capture controller ended\n',encoding='utf-8',newline='\n')
