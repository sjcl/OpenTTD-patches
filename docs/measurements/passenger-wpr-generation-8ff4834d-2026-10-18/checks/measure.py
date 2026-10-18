"""Measure one ordinary 2048-tick replay per mode; canonical equality is not required."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OLD=ROOT/'build-rebaseline-88cb61e3'
sys.path.insert(0,str(ROOT/'utils'))
import passenger_precomputed_real_map as real
assert (WORK/'build-ready.txt').is_file()
seeds=json.loads((WORK/'input-conversion-validation.json').read_text())
assert all(s['canonical_fields_equal']==22 and s['consolidated_database_wire_identical'] for s in seeds.values())
env={k:v for k,v in os.environ.items() if not k.startswith('OPENTTD_')}
checks={}
for mode in ['legacy','hyperpath']:
    package=WORK/'inputs'/mode
    with (package/'input.sav').open('rb') as stream:
        assert hashlib.file_digest(stream,'sha256').hexdigest()==seeds[mode]['input_sha256']
    deadline=time.monotonic()+2400
    while True:
        real.PROFILE.wait_for_idle(max(0,deadline-time.monotonic()),60)
        nodes=real.PROFILE.powershell("Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'MSBuild.exe' -and $_.CommandLine -match '/nodemode:' } | ForEach-Object { $_.ProcessId }").strip()
        if not nodes:break
        if time.monotonic()>deadline:raise TimeoutError('MSBuild nodes remain; strict kernel process audit would reject the profile')
        print('Waiting for MSBuild nodes: '+nodes.replace('\n',', '),flush=True)
        time.sleep(10)
    print('Beginning '+mode+' ordinary WPR',flush=True)
    with (WORK/(mode+'-runner.log')).open('wb') as out:
        subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',
            str(WORK/'Release/openttd.exe'),str(package),str(WORK/mode),
            '--ticks','2048','--timeout','2400','--measurement-wait','2400','--resume','--precomputed',
            '--no-hyperpath' if mode=='legacy' else '--hyperpath','--warm-precomputed','--wait-background-ready',
            '--hyperpath-filter-mode','index','--main-thread-affinity-mask','1','--lang',str(WORK/'lang'),'--wpr-profile'],
            cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
    meta=json.loads((WORK/mode/'process.json').read_text());clock=meta['actual-ticks.json']
    assert meta['exit_code']==0 and meta['simulation_timing_valid'] and not meta['load_failed']
    assert meta['save-integrity.json']['payments_valid']
    assert clock['start_tick']==13647872 and clock['end_tick']==13649920 and len(clock['samples'])==2048
    assert meta['input_sha256']==seeds[mode]['input_sha256']
    assert meta['resume_settings'] and meta['warm_complete'] and not meta['rebuild_precomputed'] and not meta['cold_precomputed']
    checks[mode]=dict(exact_ticks=2048,input_sha256=meta['input_sha256'],simulation_timing_valid=True,canonical_equality_required=False)
    (WORK/'runs-complete.json').write_text(json.dumps(checks,indent=2)+'\n')
    print('Completed '+mode+'; exact 2048 ticks; changed canonical permitted',flush=True)
print('Both ordinary WPR replays completed',flush=True)
