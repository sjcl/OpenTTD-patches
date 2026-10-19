"""Compare canonical states, requested inclusives, and vector reallocation under StepHyperpath."""
from pathlib import Path
from collections import defaultdict
import csv
import gzip
import hashlib
import json
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-coach-memo-0b8a21c0-2026-10-19'
OUT=ROOT/'docs/measurements'/NAME
HIST=ROOT/'docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
BASE=OUT/'control-97cdc0ad'
sys.path.insert(0,str(ROOT/'utils'))
from passenger_wpr_control import SPLIT
STEP='openttd.exe!PassengerPrecomputedTable::StepHyperpath'
MAY='openttd.exe!PassengerJourneyLoadingContext::MayUseHyperpathPolicy'
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def functions(p):
    with p.open(newline='',encoding='utf-8-sig') as f:return {r['function']:r for r in csv.DictReader(f) if r['scope']=='simulation_stack'}
def percent(a,b):return 100*(b/a-1) if a else None
inputs=load(WORK/'input-identity.json');canonical={};references={}
for mode in ['legacy','hyperpath']:
    old=load(BASE/mode/'save-integrity.json');new=load(OUT/mode/'save-integrity.json')
    assert len(old)==len(new)==24
    differences={k:[old.get(k),new.get(k)] for k in sorted(old.keys()|new.keys()) if old.get(k)!=new.get(k)}
    canonical[mode]=dict(equal=not differences,fields=24,differences=differences)
    a=load(BASE/mode/'process.json');b=load(OUT/mode/'process.json')
    for key in ['input_sha256','requested_ticks','resume_settings','precomputed','hyperpath','hyperpath_filter_mode','main_thread_affinity_mask',
        'warm_precomputed','cold_precomputed','rebuild_precomputed','full_rebuild_precomputed']:
        assert a[key]==b[key],(mode,key)
    for meta in [a,b]:
        ticks=meta['actual-ticks.json']
        assert ticks['start_tick']==inputs['start_tick'] and ticks['end_tick']==inputs['target_end_tick'] and len(ticks['samples'])==2048
    for p in [BASE/mode/'save-integrity.json',BASE/mode/'process.json',BASE/(mode+'-functions.csv')]:references[p.relative_to(ROOT).as_posix()]=sha(p)
assert canonical==load(WORK/'canonical-comparison.json')
(OUT/'canonical-comparison.json').write_text(json.dumps(canonical,indent=2)+'\n')
with (OUT/'machine-control.json').open('wb') as out:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_control.py'),str(BASE),str(OUT)],cwd=ROOT,stdout=out,check=True)
control=load(OUT/'machine-control.json');rows=[];focus={}
for mode in ['legacy','hyperpath']:
    old=functions(BASE/(mode+'-functions.csv'));new=functions(OUT/(mode+'-functions.csv'))
    factor=control[mode]['machine_factor'];focus[mode]={}
    walks=[name for name in old if name.startswith('openttd.exe!StationCargoList::VisitBoardingCandidates<') and 'CargoLoad' in name]
    new_walks=[name for name in new if name.startswith('openttd.exe!StationCargoList::VisitBoardingCandidates<') and 'CargoLoad' in name]
    assert len(walks)==len(new_walks)==1 and walks==new_walks,(mode,walks,new_walks)
    labels={MAY:'MayUseHyperpathPolicy',STEP:'StepHyperpath',walks[0]:'loading_walk'}
    if mode=='hyperpath':assert all(name in old and name in new for name in labels),(mode,labels)
    for name in sorted(old.keys()|new.keys()|labels.keys()):
        a=old.get(name,{});b=new.get(name,{})
        row=dict(mode=mode,function=name,machine_factor=factor,canonical_equal=canonical[mode]['equal'],
            logging_mode_old='File',logging_mode_new='File',matched_cpu_only_profile=True)
        for kind in ['inclusive','exclusive']:
            x=float(a.get(kind+'_ms_per_tick',0));y=float(b.get(kind+'_ms_per_tick',0))
            row.update({kind+'_old_ms_per_tick':x,kind+'_new_ms_per_tick':y,kind+'_raw_percent':percent(x,y),
                kind+'_new_adjusted_ms_per_tick':y/factor,kind+'_adjusted_percent':percent(x,y/factor),
                kind+'_old_samples':int(a.get(kind+'_samples',0)),kind+'_new_samples':int(b.get(kind+'_samples',0))})
        rows.append(row)
        if name in labels:focus[mode][labels[name]]=row
with (OUT/'function-before-after.csv').open('w',newline='',encoding='utf-8') as f:
    w=csv.DictWriter(f,list(rows[0]));w.writeheader();w.writerows(rows)
def reallocations(path):
    total_ms=0;total_samples=0;detail=defaultdict(lambda:[0,0])
    with gzip.open(path,'rt',encoding='utf-8',errors='replace',newline='') as stream:
        rows=csv.reader(stream);next(rows)
        for row in rows:
            if len(row)<5 or not row[1] or not row[2]:continue
            frames=SPLIT.split(row[2])
            if 'openttd.exe!StateGameLoop' not in frames or STEP not in frames:continue
            matches=[frame for frame in frames[frames.index(STEP)+1:] if frame.startswith('openttd.exe!std::vector<') and 'realloc' in frame.lower()]
            if not matches:continue
            # Count a stack once even if it contains nested vector reallocation frames.
            ms=float(row[3].replace(',',''));samples=int(row[4].replace(',',''))
            total_ms+=ms;total_samples+=samples
            detail[matches[0]][0]+=ms;detail[matches[0]][1]+=samples
    return dict(inclusive_ms_per_tick=total_ms/2048,samples=total_samples,
        frames={k:dict(inclusive_ms_per_tick=v[0]/2048,samples=v[1]) for k,v in detail.items()})
reallocation={}
for mode in ['legacy','hyperpath']:
    old=reallocations(BASE/mode/'CPU_Usage_(Sampled)_Stacks.csv.gz');new=reallocations(OUT/mode/'CPU_Usage_(Sampled)_Stacks.csv.gz')
    factor=control[mode]['machine_factor'];x=old['inclusive_ms_per_tick'];y=new['inclusive_ms_per_tick']
    reallocation[mode]=dict(old=old,new=new,new_adjusted_ms_per_tick=y/factor,raw_percent=percent(x,y),adjusted_percent=percent(x,y/factor),
        predicate='StateGameLoop and StepHyperpath exact frames, then std::vector frame containing realloc. Each stack counted once.')
(OUT/'step-vector-reallocation.json').write_text(json.dumps(reallocation,indent=2)+'\n')
(OUT/'focus.json').write_text(json.dumps(focus,indent=2)+'\n')
historical={}
for mode in ['legacy','hyperpath']:
    funcs=functions(HIST/(mode+'-functions.csv'))
    historical[mode]={}
    for label,row in focus[mode].items():
        name=row['function'];entry=funcs.get(name,{})
        historical[mode][label]=dict(function=name,inclusive_ms_per_tick=float(entry.get('inclusive_ms_per_tick',0)),
            inclusive_samples=int(entry.get('inclusive_samples',0)),recording_profile='Historical full CPU profile; different from matched CPU-only controls')
    historical[mode]['vector_reallocation']=reallocations(HIST/mode/'CPU_Usage_(Sampled)_Stacks.csv.gz')
(OUT/'historical-focus.json').write_text(json.dumps(historical,indent=2)+'\n')
(OUT/'comparison.json').write_text(json.dumps(dict(baseline_commit='97cdc0ad4c9493daea0db6079e90a03924d96b56',
    current_commit='0b8a21c0f3dc9a24ca0d9d3581c19fb9c207e684',same_input=True,input_sha256=inputs['input_sha256'],canonical=canonical,
    correction_is_estimate=True,recorder_settings=load(WORK/'recorder-settings.json'),reference_files=references,focus=focus,
    step_vector_reallocation=reallocation),indent=2)+'\n')
print(json.dumps(dict(canonical=canonical,focus=focus,reallocation=reallocation),indent=2),flush=True)
