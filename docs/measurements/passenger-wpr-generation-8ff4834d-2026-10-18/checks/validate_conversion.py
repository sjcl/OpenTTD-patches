from pathlib import Path
import hashlib
import json
import lzma
import os
import re
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
seeds=json.loads((ROOT/'build-rebaseline-88cb61e3/seed-validation.json').read_text())
conversion=json.loads((WORK/'input-conversion.json').read_text())
env={k:v for k,v in os.environ.items() if not k.startswith('OPENTTD_')}
results={}
for mode in ['legacy','hyperpath']:
    output=WORK/('conversion-validation-'+mode)
    command=[sys.executable,'-B',str(ROOT/'utils/passenger_precomputed_real_map.py'),'run',
        str(WORK/'Release/openttd.exe'),str(WORK/'inputs'/mode),str(output),'--ticks','0','--timeout','900',
        '--validation-only','--resume','--precomputed','--no-hyperpath' if mode=='legacy' else '--hyperpath',
        '--warm-precomputed','--wait-background-ready','--hyperpath-filter-mode','index',
        '--main-thread-affinity-mask','1','--lang',str(WORK/'lang')]
    if not (output/'process.json').exists():
        with (WORK/(mode+'-conversion-validation.log')).open('wb') as stream:
            subprocess.run(command,cwd=ROOT,env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)
    meta=json.loads((output/'process.json').read_text());state=meta['save-integrity.json']
    assert meta['exit_code']==0 and not meta['load_failed'] and meta['warm_complete']
    assert meta['requested_ticks']==0 and meta['validation_only'] and state['state_ticks']==13647872
    assert meta['background_ready']['tick_before']==meta['background_ready']['tick_after']==13647872
    ticks=re.findall(r'^State ticks: (\d+)',(output/'on-1-console.txt').read_text(),re.M)
    assert len(ticks)==2 and set(ticks)=={'13647872'},ticks
    # These two values encode format numbering. All actual packet/payment,
    # ownership/loading, rows/Work and delivery fields must remain unchanged.
    excluded={'runtime_routing_feature','database_digest'}
    old=seeds[mode]['canonical']
    differences={k:[old[k],state.get(k)] for k in old if k not in excluded and old[k]!=state.get(k)}
    assert not differences,differences
    raw=(output/'result.sav').read_bytes();assert raw[:4]==b'OTTX'
    data=lzma.decompress(raw[8:]);at=data.rindex(b'JPSD');riff=int.from_bytes(data[at+4:at+8],'big')
    length=(riff&0xFFFFFF)|((riff>>28)<<24);wire=data[at+8:at+8+length]
    expected=(WORK/(mode+'-converted-jpsd.bin')).read_bytes()
    assert wire==expected,'Current loader changed converted database bytes'
    results[mode]=dict(state_ticks_advanced=0,state_ticks=13647872,canonical_fields_equal=len(old)-len(excluded),
        excluded_format_fields=sorted(excluded),actual_state_differences=differences,
        consolidated_database_wire_identical=True,database_sha256=hashlib.sha256(wire).hexdigest(),
        input_sha256=conversion[mode]['input_sha256'],source_input_sha256=conversion[mode]['source_input_sha256'])
    print(mode,results[mode],flush=True)
(WORK/'input-conversion-validation.json').write_text(json.dumps(results,indent=2)+'\n')
