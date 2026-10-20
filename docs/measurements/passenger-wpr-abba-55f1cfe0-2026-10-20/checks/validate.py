from pathlib import Path
import sys,subprocess,os,json,re,hashlib
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
INPUT=ROOT/'build-wpr-clang-thin-2026-10-19/input'
import time
for marker in ['build-ready.txt','symbols-ready.txt']:
 until=time.monotonic()+7200
 while not (WORK/marker).is_file():
  if time.monotonic()>until:raise TimeoutError(marker)
  time.sleep(1)
command=[sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',str(WORK/'Release/openttd.exe'),str(INPUT),str(WORK/'validate'),
 '--ticks','2048','--timeout','2400','--measurement-wait','2400','--resume','--precomputed','--hyperpath','--warm-precomputed','--wait-background-ready',
 '--hyperpath-filter-mode','validate','--main-thread-affinity-mask','1','--lang',str(WORK/'lang'),'--wait-saved-replay','--validation-only']
assert not (WORK/'validate').exists()
env={k:v for k,v in os.environ.items() if not k.startswith(('OPENTTD_','LLVM_PROFILE_'))}
with (WORK/'validate-runner.log').open('wb') as f:result=subprocess.run(command,cwd=ROOT,env=env,stdout=f,stderr=subprocess.STDOUT)
meta=json.loads((WORK/'validate/process.json').read_text(encoding='utf-8-sig'))
console=(WORK/'validate/on-1-console.txt').read_text(encoding='utf-8-sig');diagnostics={}
for category in ['memo','lookup']:
 lines=re.findall(r'^\s*Passenger hyperpath '+category+r': (.+)$',console,re.MULTILINE);assert len(lines)==1
 diagnostics[category]={k:int(v) for k,v in (f.split('=',1) for f in lines[0].strip().split('; '))}
canonical=json.loads((ROOT/'docs/measurements/passenger-wpr-abba-6c11c67f-2026-10-19/02-B1/hyperpath/save-integrity.json').read_text(encoding='utf-8-sig'))
actual=meta['save-integrity.json'];diff={k:[canonical.get(k),actual.get(k)] for k in canonical.keys()|actual.keys() if canonical.get(k)!=actual.get(k)}
ticks=meta['actual-ticks.json']
quality=dict(exit_code=result.returncode,canonical_equal=not diff,canonical_fields=len(actual),differences=diff,diagnostics=diagnostics,
 exact_ticks=len(ticks['samples'])==2048 and ticks['start_tick']==3197276448 and ticks['end_tick']==3197278496,
 saved_replay=meta['saved_replay'],warm_complete=meta['warm_complete'],background_ready=meta['background_ready'],validation_only=meta['validation_only'],
 simulation_timing_valid=meta['simulation_timing_valid'],command=command)
identity=json.loads((WORK/'build-identity.json').read_text(encoding='utf-8'))
inputs=json.loads((WORK/'input-identity.json').read_text(encoding='utf-8'))
assert meta['input_sha256']==inputs['input_sha256'] and meta['binary_sha256']==identity['files']['Release/openttd.exe']['sha256']
assert meta['background_ready']['tick_before']==meta['background_ready']['tick_after']==3197276448 and actual['payments_valid']
quality['counters_passed']=diagnostics['memo']['validated']>0 and diagnostics['lookup']['validated_groups']>0 and diagnostics['memo']['violations']==diagnostics['memo']['lookup_duplicates']==diagnostics['lookup']['violations']==0
quality['passed']=result.returncode==0 and quality['canonical_equal'] and quality['canonical_fields']==24 and quality['exact_ticks'] and quality['warm_complete'] and quality['saved_replay']['ready'] and quality['background_ready']['completed'] and diagnostics['memo']['validated']>0 and diagnostics['lookup']['validated_groups']>0 and diagnostics['memo']['violations']==diagnostics['memo']['lookup_duplicates']==diagnostics['lookup']['violations']==0
(WORK/'validate-result.json').write_text(json.dumps(quality,indent=2)+'\n',encoding='utf-8',newline='\n')
print(json.dumps(quality,indent=2),flush=True)
assert quality['passed'],'Validation failed; retained evidence, do not retry'
