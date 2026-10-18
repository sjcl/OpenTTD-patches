"""Validate both modes from the requested unmodified input without advancing ticks."""
from pathlib import Path
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
SOURCE=Path('C:/Users/bebe/OneDrive/ドキュメント/OpenTTD/save/gc3833d807.sav')
EXPECTED='916cfa88eab18342199ec0c77f0c15c5477c961be7fa60d71e2d22a1b878992a'
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
deadline=time.monotonic()+3600
while not (WORK/'build-ready.txt').is_file():
    if time.monotonic()>deadline:raise TimeoutError('Build not ready')
    time.sleep(2)
package=WORK/'input';manifest=load(package/'package.json')
assert sha(SOURCE)==sha(package/'input.sav')==manifest['input_sha256']==EXPECTED
(package/'input.sav').chmod(stat.S_IREAD)
env={k:v for k,v in os.environ.items() if not k.startswith('OPENTTD_')}
results={}
for mode in ['legacy','hyperpath']:
    output=WORK/('validation-'+mode)
    command=[sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',
        str(WORK/'Release/openttd.exe'),str(package),str(output),'--ticks','0','--timeout','1200',
        '--validation-only','--resume','--precomputed','--no-hyperpath' if mode=='legacy' else '--hyperpath',
        '--warm-precomputed','--wait-background-ready','--hyperpath-filter-mode','index',
        '--main-thread-affinity-mask','1','--lang',str(WORK/'lang')]
    with (WORK/(mode+'-validation.log')).open('wb') as stream:
        subprocess.run(command,cwd=ROOT,env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)
    meta=load(output/'process.json');state=meta['save-integrity.json']
    assert meta['exit_code']==0 and not meta['load_failed'] and meta['warm_complete']
    assert meta['requested_ticks']==0 and meta['validation_only'] and state['payments_valid']
    assert meta['input_sha256']==EXPECTED
    ticks=list(map(int,re.findall(r'^State ticks: (\d+)',(output/'on-1-console.txt').read_text(encoding='utf-8-sig'),re.M)))
    assert len(ticks)==2 and ticks[0]==ticks[1]==state['state_ticks'],ticks
    assert meta['background_ready']['completed']
    assert meta['background_ready']['tick_before']==meta['background_ready']['tick_after']==ticks[0]
    assert not meta['cold_precomputed'] and not meta['rebuild_precomputed'] and not meta['full_rebuild_precomputed']
    results[mode]=dict(input_sha256=EXPECTED,start_tick=ticks[0],state_ticks_advanced=0,
        load_failed=False,payments_valid=True,warm_complete=True,background_ready=meta['background_ready'],
        command=command,canonical_endpoint_equality_required=False)
    print(mode,results[mode],flush=True)
assert results['legacy']['start_tick']==results['hyperpath']['start_tick']
identity=dict(source=str(SOURCE),source_bytes=SOURCE.stat().st_size,source_sha256=EXPECTED,
    frozen_input_sha256=sha(package/'input.sav'),copied_without_conversion=True,
    config_sha256=sha(package/'openttd.cfg'),asset_files=len(manifest['assets']),
    asset_bytes=sum(p['bytes'] for p in manifest['assets']),start_tick=results['legacy']['start_tick'],
    target_end_tick=results['legacy']['start_tick']+2048,runs=results)
(WORK/'input-validation.json').write_text(json.dumps(identity,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print('Both zero-tick loads ready; original input used directly for profiles',flush=True)
