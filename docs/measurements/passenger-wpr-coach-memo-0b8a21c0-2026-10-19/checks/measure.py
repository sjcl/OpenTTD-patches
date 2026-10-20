"""Replay each mode for 2048 ticks and compare all 24 canonical fields to 97cdc0ad."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
BASE=ROOT/'docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
sys.path.insert(0,str(ROOT/'utils'))
import passenger_precomputed_real_map as real
deadline=time.monotonic()+7200
while not ((WORK/'input-identity.json').is_file() and (WORK/'build-ready.txt').is_file()):
    if time.monotonic()>deadline:raise TimeoutError('Build/input not ready')
    time.sleep(2)
inputs=json.loads((WORK/'input-identity.json').read_text(encoding='utf-8'))
assert inputs['copied_without_conversion'] and inputs['same_as_97cdc0ad']
env={k:v for k,v in os.environ.items() if not k.startswith('OPENTTD_')}
checks={};canonical={}
for mode in ['legacy','hyperpath']:
    package=WORK/'input'
    with (package/'input.sav').open('rb') as stream:
        assert hashlib.file_digest(stream,'sha256').hexdigest()==inputs['input_sha256']
    deadline=time.monotonic()+2400
    while True:
        real.PROFILE.wait_for_idle(max(0,deadline-time.monotonic()),60)
        nodes=real.PROFILE.powershell("Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'MSBuild.exe' -and $_.CommandLine -match '/nodemode:' } | ForEach-Object { $_.ProcessId }").strip()
        if not nodes:break
        if time.monotonic()>deadline:raise TimeoutError('MSBuild nodes remain')
        print('Waiting for MSBuild nodes: '+nodes.replace('\n',', '),flush=True)
        time.sleep(10)
    print('Beginning '+mode+' ordinary WPR',flush=True)
    with (WORK/(mode+'-runner.log')).open('wb') as out:
        subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',
            str(WORK/'Release/openttd.exe'),str(package),str(WORK/mode),
            '--ticks','2048','--timeout','2400','--measurement-wait','2400','--resume','--wait-saved-replay','--precomputed',
            '--no-hyperpath' if mode=='legacy' else '--hyperpath','--warm-precomputed','--wait-background-ready',
            '--hyperpath-filter-mode','index','--main-thread-affinity-mask','1','--lang',str(WORK/'lang'),'--wpr-profile'],
            cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
    meta=json.loads((WORK/mode/'process.json').read_text());clock=meta['actual-ticks.json']
    assert meta['exit_code']==0 and meta['simulation_timing_valid'] and not meta['load_failed']
    assert meta['save-integrity.json']['payments_valid']
    assert clock['start_tick']==inputs['start_tick'] and clock['end_tick']==inputs['target_end_tick'] and len(clock['samples'])==2048
    assert meta['input_sha256']==inputs['input_sha256']
    assert meta['resume_settings'] and meta['warm_complete'] and not meta['rebuild_precomputed'] and not meta['cold_precomputed']
    old=json.loads((BASE/mode/'save-integrity.json').read_text())
    actual=meta['save-integrity.json'];assert len(old)==len(actual)==24
    differences={k:[old.get(k),actual.get(k)] for k in sorted(old.keys()|actual.keys()) if old.get(k)!=actual.get(k)}
    canonical[mode]=dict(equal=not differences,fields=24,differences=differences)
    (WORK/'canonical-comparison.json').write_text(json.dumps(canonical,indent=2)+'\n')
    checks[mode]=dict(exact_ticks=2048,input_sha256=meta['input_sha256'],simulation_timing_valid=True,
        canonical_equality_required=True,canonical_equal=not differences)
    (WORK/'runs-complete.json').write_text(json.dumps(checks,indent=2)+'\n')
    print('Completed '+mode+'; canonical '+('24/24 equal' if not differences else json.dumps(differences)),flush=True)
print('Both ordinary WPR replays completed',flush=True)
