"""Compare exact functions and ordered call paths; count each sampled stack once."""
from pathlib import Path
from collections import defaultdict
from decimal import Decimal
import csv
import gzip
import hashlib
import json
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OUT=ROOT/'docs/measurements/passenger-wpr-clang-thin-merged-2026-10-19'
BASE=ROOT/'docs/measurements/passenger-wpr-loading-scratch-8f829b8c-2026-10-19'
sys.path.insert(0,str(ROOT/'utils'))
from passenger_wpr_control import SPLIT
TICKS=2048
LOAD='openttd.exe!StationCargoList::Load'
MAY='openttd.exe!PassengerJourneyLoadingContext::MayUseHyperpathPolicy'
CAN='openttd.exe!PassengerJourneyLoadingContext::CanUseHyperpathPolicy'
PLAN='openttd.exe!PassengerServiceDatabase::PlanPrecomputed'
CTOR='openttd.exe!PassengerJourneyLoadingContext::PassengerJourneyLoadingContext'
DTOR='openttd.exe!PassengerJourneyLoadingContext::~PassengerJourneyLoadingContext'
MALLOC='openttd.exe!_malloc_base'
FREE='ntdll.dll!RtlFreeHeap'
RETIRED='openttd.exe!PassengerJourneyLoadingContext::RetiredPlan'

def load(path):return json.loads(path.read_text(encoding='utf-8-sig'))
def dump(path,value):path.write_text(json.dumps(value,indent=2,ensure_ascii=False)+'\n',encoding='utf-8',newline='\n')
def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def percent(old,new):return 100*(new/old-1) if old else None
def functions(path):
    with path.open(encoding='utf-8-sig',newline='') as stream:
        return {r['function']:r for r in csv.DictReader(stream) if r['scope']=='simulation_stack'}
def ordered(frames,parent,child):
    return parent in frames and child in frames[frames.index(parent)+1:]

def path_metrics(evidence,mode):
    totals=defaultdict(lambda:[0,0]);present=set();metadata=load(evidence/mode/'process.json')
    tid=metadata['main_thread']['thread_id']
    with gzip.open(evidence/mode/'CPU_Usage_(Sampled)_Stacks.csv.gz','rt',encoding='utf-8-sig',newline='') as stream:
        for row in csv.DictReader(stream):
            if not row['Thread ID'] or int(row['Thread ID'])!=tid or not row['Stack']:continue
            frames=SPLIT.split(row['Stack'])
            if 'openttd.exe!StateGameLoop' not in frames:continue
            ns=int(Decimal(row['Weight (ms)'].replace(',',''))*1000000)
            count=int(row['Count'].replace(',',''));present.update(frames)
            checks={
                'StationCargoList::Load':LOAD in frames,
                'Load -> _malloc_base':ordered(frames,LOAD,MALLOC),
                'Load -> RtlFreeHeap':ordered(frames,LOAD,FREE),
                'LoadingContext constructor':CTOR in frames,
                'LoadingContext destructor':DTOR in frames,
                'Load -> LoadingContext constructor':ordered(frames,LOAD,CTOR),
                'Load -> LoadingContext destructor':ordered(frames,LOAD,DTOR),
                'MayUseHyperpathPolicy':MAY in frames,
                'MayUseHyperpathPolicy -> PlanPrecomputed':ordered(frames,MAY,PLAN),
                'CanUseHyperpathPolicy -> PlanPrecomputed':ordered(frames,CAN,PLAN),
                'PlanPrecomputed':PLAN in frames,
                'RetiredPlan':RETIRED in frames,
                'loading walk':any(f.startswith('openttd.exe!StationCargoList::VisitBoardingCandidates<') and 'CargoLoad' in f for f in frames),
            }
            for label,match in checks.items():
                if match:totals[label][0]+=ns;totals[label][1]+=count
    labels=['StationCargoList::Load','Load -> _malloc_base','Load -> RtlFreeHeap','LoadingContext constructor','LoadingContext destructor',
            'Load -> LoadingContext constructor','Load -> LoadingContext destructor','MayUseHyperpathPolicy',
            'MayUseHyperpathPolicy -> PlanPrecomputed','CanUseHyperpathPolicy -> PlanPrecomputed','PlanPrecomputed','RetiredPlan','loading walk']
    return {label:dict(ns=totals[label][0],samples=totals[label][1],ms_per_tick=totals[label][0]/1000000/TICKS) for label in labels},present

assert (WORK/'post-complete.txt').is_file()
captures=load(WORK/'capture-results.json');summary=load(OUT/'summary.json')
rows=[];focus={};same_settings={};references={};profile_hashes={}
for name in ['cpu-minimal.wprp','cpu-sampled.wpaProfile']:
    assert sha(BASE/name)==sha(WORK/name)
    profile_hashes[name]=dict(old=sha(BASE/name),new=sha(WORK/name))
for mode in ['legacy','hyperpath']:
    old_meta=load(BASE/mode/'process.json');new_meta=load(OUT/mode/'process.json')
    fields=['input_sha256','requested_ticks','resume_settings','precomputed','hyperpath','hyperpath_filter_mode','main_thread_affinity_mask',
            'warm_precomputed','cold_precomputed','rebuild_precomputed','full_rebuild_precomputed','validation_only']
    assert all(old_meta[k]==new_meta[k] for k in fields)
    assert summary[mode]['sampled_cpu_ms_per_tick']['simulation_stack']>0
    assert not summary[mode]['unresolved_product_functions']
    same_settings[mode]={key:new_meta[key] for key in fields}
    old_functions=functions(BASE/(mode+'-functions.csv'));new_functions=functions(OUT/(mode+'-functions.csv'))
    old_paths,_=path_metrics(BASE,mode);new_paths,_=path_metrics(OUT,mode)
    focus[mode]={label:dict(old=old_paths[label],new=new_paths[label],raw_percent=percent(old_paths[label]['ms_per_tick'],new_paths[label]['ms_per_tick'])) for label in old_paths}
    if new_paths['loading walk']['samples']==0:
        focus[mode]['loading walk'].update(comparable=False,raw_percent=None,
            reason='Clang inlined the visitor template body; remaining source-position lambda symbols do not provide the old CargoLoad body boundary.')
    for function in sorted(old_functions.keys()|new_functions.keys()):
        old,new=old_functions.get(function,{}),new_functions.get(function,{})
        entry=dict(mode=mode,function=function)
        for kind in ['inclusive','exclusive']:
            a=float(old.get(kind+'_ms_per_tick',0));b=float(new.get(kind+'_ms_per_tick',0))
            entry.update({kind+'_old_ms_per_tick':a,kind+'_new_ms_per_tick':b,kind+'_raw_percent':percent(a,b),
                          kind+'_old_samples':int(old.get(kind+'_samples',0)),kind+'_new_samples':int(new.get(kind+'_samples',0))})
        rows.append(entry)
    for path in [BASE/mode/'save-integrity.json',BASE/mode/'process.json',BASE/(mode+'-functions.csv'),BASE/mode/'CPU_Usage_(Sampled)_Stacks.csv.gz']:
        references[path.relative_to(ROOT).as_posix()]=sha(path)
    console=gzip.decompress((BASE/mode/'on-1-console.txt.gz').read_bytes()).decode('utf-8-sig')
    import re
    memo=re.findall(r'^\s*Passenger hyperpath memo: (.+)$',console,re.MULTILINE);assert len(memo)==1
    captures[mode]['baseline_memo']={k:int(v) for k,v in (field.split('=',1) for field in memo[0].strip().split('; '))}
with (OUT/'function-before-after.csv').open('w',encoding='utf-8',newline='') as stream:
    writer=csv.DictWriter(stream,list(rows[0]),lineterminator='\n');writer.writeheader();writer.writerows(rows)
dump(OUT/'focus.json',focus)
dump(OUT/'canonical-comparison.json',{mode:data['canonical'] for mode,data in captures.items()})
dump(OUT/'comparison.json',dict(baseline_commit='8f829b8c3d5f35cb420c2a16812716ef24e38939',current_commit='f97bac717c840852d8240f0eac51a92bad0abb9b',
     same_settings=same_settings,matched_cpu_only_profile=True,profile_hashes=profile_hashes,reference_files=references,capture_results=captures,
     compiler_and_build_configuration_changed=True,untouched_correction_used=False,
     reason='Compiler/LTO/assert changes affect all code, so untouched samples cannot serve as an unchanged-code drift control.',
     attribution='Combined jgrpp-ya merge, compiler, assert-off and ThinLTO configuration; not isolated compiler-only gain.',
     method='Main-thread StateGameLoop samples. Function/path boundaries can change through inlining/internalization; world CPU is primary.'))
(WORK/'analysis-complete.txt').write_text('Raw CPU/function/path comparison and canonical checks complete\n',encoding='utf-8',newline='\n')
print(json.dumps(dict(focus=focus,canonical={mode:data['canonical'] for mode,data in captures.items()}),indent=2),flush=True)
