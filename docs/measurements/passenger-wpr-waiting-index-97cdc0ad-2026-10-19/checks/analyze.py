"""Compare all canonical fields and inclusive/exclusive function times to 9ab0228d."""
from pathlib import Path
import csv
import hashlib
import json
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
OUT=ROOT/'docs/measurements'/NAME
BASE=ROOT/'docs/measurements/passenger-wpr-local-policy-9ab0228d-2026-10-18'
FOCUS=['openttd.exe!StationCargoList::RoutePassengerJourneys', 'openttd.exe!StationCargoList::RecordCapacityMiss', 'openttd.exe!StationCargoList::ForgetWaitingPacket', 'openttd.exe!StationCargoList::IndexPacket', 'openttd.exe!StationCargoList::AddBoardingEntry', 'openttd.exe!StationCargoList::RemoveBoardingEntry']

def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def functions(p):
    with p.open(newline='',encoding='utf-8-sig') as f:return {r['function']:r for r in csv.DictReader(f) if r['scope']=='simulation_stack'}
def percent(a,b):return 100*(b/a-1) if a else None
inputs=load(WORK/'input-identity.json')
canonical={};references={}
for mode in ['legacy','hyperpath']:
    old=load(BASE/mode/'save-integrity.json');new=load(OUT/mode/'save-integrity.json')
    assert len(old)==len(new)==24
    differences={k:[old.get(k),new.get(k)] for k in sorted(old.keys()|new.keys()) if old.get(k)!=new.get(k)}
    canonical[mode]=dict(equal=not differences,fields=24,differences=differences)
    a=load(BASE/mode/'process.json');b=load(OUT/mode/'process.json')
    for key in ['input_sha256','requested_ticks','resume_settings','precomputed','hyperpath','hyperpath_filter_mode',
        'main_thread_affinity_mask','warm_precomputed','cold_precomputed','rebuild_precomputed','full_rebuild_precomputed']:
        assert a[key]==b[key],(mode,key)
    for meta in [a,b]:
        ticks=meta['actual-ticks.json']
        assert ticks['start_tick']==inputs['start_tick'] and ticks['end_tick']==inputs['target_end_tick'] and len(ticks['samples'])==2048
    for p in [BASE/mode/'save-integrity.json',BASE/mode/'process.json',BASE/(mode+'-functions.csv')]:
        references[p.relative_to(ROOT).as_posix()]=sha(p)
assert canonical==load(WORK/'canonical-comparison.json')
(OUT/'canonical-comparison.json').write_text(json.dumps(canonical,indent=2)+'\n')
with (OUT/'machine-control.json').open('wb') as out:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_control.py'),str(BASE),str(OUT)],cwd=ROOT,stdout=out,check=True)
control=load(OUT/'machine-control.json');rows=[];focus={}
for mode in ['legacy','hyperpath']:
    old=functions(BASE/(mode+'-functions.csv'));new=functions(OUT/(mode+'-functions.csv'))
    factor=control[mode]['machine_factor'];focus[mode]={}
    assert FOCUS[0] in old and FOCUS[0] in new,(mode,FOCUS[0])
    for name in sorted(old.keys()|new.keys()|set(FOCUS)):
        a=old.get(name,{});b=new.get(name,{})
        row=dict(mode=mode,function=name,machine_factor=factor,canonical_equal=canonical[mode]['equal'],
            logging_mode_old='File' if mode=='hyperpath' else 'Memory',logging_mode_new='File' if mode=='hyperpath' else 'Memory',
            event_buffers_baseline_default=20,event_buffers_current=20)
        for kind in ['inclusive','exclusive']:
            x=float(a.get(kind+'_ms_per_tick',0));y=float(b.get(kind+'_ms_per_tick',0))
            row.update({kind+'_old_ms_per_tick':x,kind+'_new_ms_per_tick':y,kind+'_raw_percent':percent(x,y),
                kind+'_new_adjusted_ms_per_tick':y/factor,kind+'_adjusted_percent':percent(x,y/factor),
                kind+'_old_samples':int(a.get(kind+'_samples',0)),kind+'_new_samples':int(b.get(kind+'_samples',0))})
        rows.append(row)
        if name in FOCUS:focus[mode][name]=row
with (OUT/'function-before-after.csv').open('w',newline='',encoding='utf-8') as f:
    w=csv.DictWriter(f,list(rows[0]));w.writeheader();w.writerows(rows)
(OUT/'focus.json').write_text(json.dumps(focus,indent=2)+'\n')
(OUT/'comparison.json').write_text(json.dumps(dict(baseline_commit='9ab0228db17178ef931de961305ebec544162f6a',
    current_commit='97cdc0ad4c9493daea0db6079e90a03924d96b56',same_input=True,input_sha256=inputs['input_sha256'],
    canonical=canonical,correction_is_estimate=True,recorder_settings=load(WORK/'recorder-settings.json'),
    reference_files=references,focus=focus),indent=2)+'\n')
print(json.dumps(dict(canonical=canonical,focus=focus),indent=2),flush=True)
