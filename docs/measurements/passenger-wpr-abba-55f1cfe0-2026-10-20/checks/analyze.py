"""ABBA means/ranges and exact requested stack boundaries, including StepImpl self."""
from pathlib import Path
from collections import defaultdict
from decimal import Decimal
import csv
import gzip
import hashlib
import json
import statistics
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OUT=ROOT/'docs/measurements/passenger-wpr-abba-55f1cfe0-2026-10-20'
BASE=ROOT/'docs/measurements/passenger-wpr-abba-6c11c67f-2026-10-19/02-B1'
sys.path.insert(0,str(ROOT/'utils'))
from passenger_wpr_control import SPLIT
ROUNDS=[('01-A1','A'),('02-B1','B'),('03-B2','B'),('04-A2','A')]
TICKS=2048
STEP='openttd.exe!PassengerPrecomputedTable::StepImpl'
RETIRED='openttd.exe!PassengerJourneyLoadingContext::RetiredPlan'
LOOKUP='openttd.exe!PassengerServiceDatabase::HyperpathLookupCandidates'
PLAN='openttd.exe!PassengerServiceDatabase::PlanPrecomputed'
SHARED='openttd.exe!PassengerServiceDatabase::SharedHyperpathPlan'
TICK='openttd.exe!PassengerServiceDatabase::TickPrecomputed'
FINISH='openttd.exe!PassengerPrecomputedTable::GraphSnapshot::PrepareHyperpathFinish'
MAY='openttd.exe!PassengerJourneyLoadingContext::MayUseHyperpathPolicy'
RESOLVE='openttd.exe!PassengerJourneyLoadingContext::ResolveHyperpath'
DBBOARD='openttd.exe!PassengerServiceDatabase::BoardHyperpath'
BOARD='openttd.exe!PassengerPrecomputedTable::Board'
LABELS=['MayUseHyperpathPolicy','MayUseHyperpathPolicy self','ResolveHyperpath','BoardHyperpath','Board','ResolveHyperpath / Board union','StateGameLoop','StepImpl self','StepImpl inclusive','RetiredPlan','RetiredPlan -> PlanPrecomputed',
        'HyperpathLookupCandidates','HyperpathLookupCandidates -> PlanPrecomputed','SharedHyperpathPlan','PlanPrecomputed',
        'SharedHyperpathPlan self','RetiredPlan -> SharedHyperpathPlan','HyperpathLookupCandidates -> SharedHyperpathPlan','TickPrecomputed','worker PrepareHyperpathFinish']

def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def dump(p,x):p.write_text(json.dumps(x,indent=2,ensure_ascii=False)+'\n',encoding='utf-8',newline='\n')
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def percent(a,b):return 100*(b/a-1) if a else None
def ordered(frames,a,b):return a in frames and b in frames[frames.index(a)+1:]
def write_csv(p,rows):
    with p.open('w',encoding='utf-8',newline='') as f:
        writer=csv.DictWriter(f,list(rows[0]),lineterminator='\n');writer.writeheader();writer.writerows(rows)
def function_rows(p):
    with p.open(encoding='utf-8-sig',newline='') as f:return {r['function']:r for r in csv.DictReader(f) if r['scope']=='simulation_stack'}
def metrics(folder,mode):
    result=defaultdict(lambda:[0,0]);tid=load(folder/mode/'process.json')['main_thread']['thread_id'];names=set()
    with gzip.open(folder/mode/'CPU_Usage_(Sampled)_Stacks.csv.gz','rt',encoding='utf-8-sig',newline='') as f:
        for row in csv.DictReader(f):
            if not row['Thread ID'] or not row['Stack']:continue
            frames=SPLIT.split(row['Stack']);main=int(row['Thread ID'])==tid
            ns=int(Decimal(row['Weight (ms)'].replace(',',''))*1000000);count=int(row['Count'].replace(',',''))
            if not main:
                if FINISH in frames:result['worker PrepareHyperpathFinish'][0]+=ns;result['worker PrepareHyperpathFinish'][1]+=count
                continue
            if 'openttd.exe!StateGameLoop' not in frames:continue
            names.update(frames)
            checks={'MayUseHyperpathPolicy':MAY in frames,'MayUseHyperpathPolicy self':frames[-1]==MAY,'ResolveHyperpath':RESOLVE in frames,'BoardHyperpath':DBBOARD in frames,'Board':BOARD in frames,'ResolveHyperpath / Board union':bool(set(frames)&{RESOLVE,DBBOARD,BOARD}),'StateGameLoop':True,'StepImpl self':frames[-1]==STEP,'StepImpl inclusive':STEP in frames,'RetiredPlan':RETIRED in frames,
                    'RetiredPlan -> PlanPrecomputed':ordered(frames,RETIRED,PLAN),'HyperpathLookupCandidates':LOOKUP in frames,
                    'HyperpathLookupCandidates -> PlanPrecomputed':ordered(frames,LOOKUP,PLAN),'SharedHyperpathPlan':SHARED in frames,
                    'PlanPrecomputed':PLAN in frames,'TickPrecomputed':TICK in frames,
                    'SharedHyperpathPlan self':frames[-1]==SHARED,'RetiredPlan -> SharedHyperpathPlan':ordered(frames,RETIRED,SHARED),
                    'HyperpathLookupCandidates -> SharedHyperpathPlan':ordered(frames,LOOKUP,SHARED)}
            for label,match in checks.items():
                if match:result[label][0]+=ns;result[label][1]+=count
    return {label:dict(ns=result[label][0],samples=result[label][1],ms_per_tick=result[label][0]/1000000/TICKS) for label in LABELS},names

assert (WORK/'post-complete.txt').is_file()
captures=load(WORK/'capture-results.json');runs={};settings={};rows=[];refs={}
fields=['input_sha256','requested_ticks','resume_settings','precomputed','hyperpath','hyperpath_filter_mode','main_thread_affinity_mask',
        'warm_precomputed','cold_precomputed','rebuild_precomputed','full_rebuild_precomputed','validation_only']
for name,label in ROUNDS:
    folder=OUT/name;summary=load(folder/'summary.json')
    for mode in ['legacy','hyperpath']:
        key=name+'/'+mode;meta=load(folder/mode/'process.json');s=summary[mode]
        expected=load(BASE/mode/'process.json')
        assert all(meta[k]==expected[k] for k in fields),key
        assert not s['unresolved_product_functions'] and s['payments_valid'] and s['ticks']==2048
        settings[mode]={k:meta[k] for k in fields}
        focus,names=metrics(folder,mode)
        # ThinLTO may inline StepHyperpath into StepImpl, but StepImpl's own boundary must remain in both builds.
        assert STEP in names and LOOKUP in names and PLAN in names and RETIRED in names,(key,names)
        functions=function_rows(folder/(mode+'-functions.csv'))
        step=functions[STEP]
        assert abs(float(step['exclusive_ms_per_tick'])-focus['StepImpl self']['ms_per_tick'])<1e-9
        runs[key]=dict(round=name,label=label,mode=mode,focus=focus,simulation_cpu_ms_per_tick=s['sampled_cpu_ms_per_tick']['simulation_stack'],
            all_threads_cpu_ms_per_tick=s['sampled_cpu_ms_per_tick']['all'],main_thread_cpu_ms_per_tick=s['sampled_cpu_ms_per_tick']['main'],
            simulation_wall_ms_per_tick=s['simulation_wall_ms_per_tick'],conservation_checks=s['conservation_checks'])
        assert abs(runs[key]['simulation_cpu_ms_per_tick']-focus['StateGameLoop']['ms_per_tick'])<1e-9
        for metric,m in focus.items():rows.append(dict(run=key,label=label,mode=mode,metric=metric,**m))
        for fn in ['save-integrity.json','process.json']:
            p=BASE/mode/fn;refs[p.relative_to(ROOT).as_posix()]=sha(p)
comparison={};function_diff=[]
for mode in ['legacy','hyperpath']:
    comparison[mode]={}
    for metric in LABELS+['simulation_wall_ms_per_tick','all_threads_cpu_ms_per_tick']:
        groups={}
        for label in ['A','B']:
            selected=[r for r in runs.values() if r['mode']==mode and r['label']==label]
            values=[r['focus'][metric]['ms_per_tick'] if metric in LABELS else r[metric] for r in selected]
            groups[label]=dict(mean=statistics.mean(values),minimum=min(values),maximum=max(values),values=values,
                              samples=sum(r['focus'][metric]['samples'] for r in selected) if metric in LABELS else None)
        a=groups['A']['mean'];b=groups['B']['mean']
        comparison[mode][metric]=dict(**groups,delta_ms_per_tick=b-a,raw_percent=percent(a,b))
    tables={name:function_rows(OUT/name/(mode+'-functions.csv')) for name,_ in ROUNDS}
    for function in sorted(set().union(*(x.keys() for x in tables.values()))):
        record=dict(mode=mode,function=function)
        for kind in ['inclusive','exclusive']:
            a=statistics.mean(float(tables[name].get(function,{}).get(kind+'_ms_per_tick',0)) for name,label in ROUNDS if label=='A')
            b=statistics.mean(float(tables[name].get(function,{}).get(kind+'_ms_per_tick',0)) for name,label in ROUNDS if label=='B')
            record.update({kind+'_A_mean_ms_per_tick':a,kind+'_B_mean_ms_per_tick':b,kind+'_delta_ms_per_tick':b-a,kind+'_percent':percent(a,b)})
        function_diff.append(record)
write_csv(OUT/'focus-runs.csv',rows);write_csv(OUT/'function-before-after.csv',function_diff)
dump(OUT/'focus.json',dict(runs=runs,comparison=comparison))
dump(OUT/'canonical-comparison.json',{key:r['canonical'] for key,r in captures.items()})
dump(OUT/'comparison.json',dict(A_commit=load(WORK/'baseline-identity.json')['product_commit'],B_commit=load(WORK/'build-identity.json')['product_commit'],
    order=[name+'/'+mode for name,_ in ROUNDS for mode in ['legacy','hyperpath']],ticks_each=2048,same_settings=settings,reference_files=refs,
    matched_clang_thinlto_conditions=True,untouched_correction_used=False,
    method='A/B means of two fresh processes per mode; ABBA order per mode. Main StateGameLoop samples; self is the stack leaf. Ordered paths count a stack once. Worker finish is a separate all-worker metric, not added to main inclusive.',
    limitations='Two repetitions per version/mode; ranges describe observed run variation, not a confidence interval. ABBA balances linear time drift but does not eliminate all noise.',comparison=comparison))
(WORK/'analysis-complete.txt').write_text('ABBA function/self/path analysis complete\n',encoding='utf-8',newline='\n')
for mode in ['legacy','hyperpath']:
    print(mode,flush=True)
    for metric in ['StateGameLoop','StepImpl self','RetiredPlan','HyperpathLookupCandidates -> PlanPrecomputed']:
        r=comparison[mode][metric];print(metric,r['A']['mean'],r['B']['mean'],r['raw_percent'],flush=True)
