"""Retake only the rejected Hyperpath performance trace; compare canonical again."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
BASE=ROOT/'docs/measurements/passenger-wpr-generation-f9c4a85b-2026-10-18'
sys.path.insert(0,str(ROOT/'utils'))
import passenger_precomputed_real_map as real
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
inputs=load(WORK/'input-identity.json');identity=load(WORK/'build-identity.json')
with (WORK/'input/input.sav').open('rb') as f:assert hashlib.file_digest(f,'sha256').hexdigest()==inputs['input_sha256']
deadline=time.monotonic()+2400
while True:
    real.PROFILE.wait_for_idle(max(0,deadline-time.monotonic()),60)
    nodes=real.PROFILE.powershell("Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'MSBuild.exe' -and $_.CommandLine -match '/nodemode:' } | ForEach-Object { $_.ProcessId }").strip()
    if not nodes:break
    if time.monotonic()>deadline:raise TimeoutError('MSBuild nodes remain')
    time.sleep(10)
env={k:v for k,v in os.environ.items() if not k.startswith('OPENTTD_')}
with (WORK/'hyperpath-retry-runner.log').open('wb') as out:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',
        str(WORK/'Release/openttd.exe'),str(WORK/'input'),str(WORK/'hyperpath'),
        '--ticks','2048','--timeout','2400','--measurement-wait','2400','--resume','--precomputed','--hyperpath',
        '--warm-precomputed','--wait-background-ready','--hyperpath-filter-mode','index',
        '--main-thread-affinity-mask','1','--lang',str(WORK/'lang'),'--wpr-profile'],
        cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
meta=load(WORK/'hyperpath/process.json');clock=meta['actual-ticks.json']
assert meta['exit_code']==0 and meta['simulation_timing_valid'] and not meta['load_failed']
assert meta['save-integrity.json']['payments_valid'] and meta['input_sha256']==inputs['input_sha256']
assert meta['binary_sha256']==identity['files']['Release/openttd.exe']['sha256']
assert clock['start_tick']==inputs['start_tick'] and clock['end_tick']==inputs['target_end_tick'] and len(clock['samples'])==2048
old=load(BASE/'hyperpath/save-integrity.json');new=meta['save-integrity.json']
assert len(old)==len(new)==24
diff={k:[old.get(k),new.get(k)] for k in sorted(old.keys()|new.keys()) if old.get(k)!=new.get(k)}
canonical=load(WORK/'canonical-comparison.json');canonical['hyperpath']=dict(equal=not diff,fields=24,differences=diff)
(WORK/'canonical-comparison.json').write_text(json.dumps(canonical,indent=2)+'\n')
runs=load(WORK/'runs-complete.json');runs['hyperpath'].update(canonical_equal=not diff,retaken_after_missing_marker=True)
(WORK/'runs-complete.json').write_text(json.dumps(runs,indent=2)+'\n')
(WORK/'hyperpath-retry-complete.txt').write_text('Retake complete; canonical compared again\n')
print(json.dumps(canonical,indent=2),flush=True)
