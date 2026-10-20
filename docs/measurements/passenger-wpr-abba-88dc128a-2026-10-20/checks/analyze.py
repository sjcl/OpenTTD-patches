"""ABBA scheduler counters and exact StateGameLoop function CPU boundaries."""
from pathlib import Path
from collections import defaultdict
from decimal import Decimal
import csv,gzip,json,statistics,sys
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OUT=ROOT/'docs/measurements/passenger-wpr-abba-88dc128a-2026-10-20'
sys.path.insert(0,str(ROOT/'utils'))
from passenger_wpr_control import SPLIT
ROUNDS=[('01-A1','A'),('02-B1','B'),('03-B2','B'),('04-A2','A')]
TICKS=2048
FRAMES={'StateGameLoop':'openttd.exe!StateGameLoop','RoutePassengerJourneys':'openttd.exe!StationCargoList::RoutePassengerJourneys','RunPassengerWaitingEvents':'openttd.exe!RunPassengerWaitingEvents'}
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def dump(p,x):p.write_text(json.dumps(x,indent=2,ensure_ascii=False)+'\n',encoding='utf-8',newline='\n')
def write_csv(p,rows):
 with p.open('w',encoding='utf-8',newline='') as f:
  w=csv.DictWriter(f,list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
def percent(a,b):return 100*(b/a-1) if a else None
def tables(p):
 with p.open(encoding='utf-8-sig',newline='') as f:return {r['function']:r for r in csv.DictReader(f) if r['scope']=='simulation_stack'}
def cpu(folder,mode):
 result=defaultdict(lambda:[0,0]);tid=load(folder/mode/'process.json')['main_thread']['thread_id']
 with gzip.open(folder/mode/'CPU_Usage_(Sampled)_Stacks.csv.gz','rt',encoding='utf-8-sig',newline='') as f:
  for row in csv.DictReader(f):
   if not row['Thread ID'] or int(row['Thread ID'])!=tid or not row['Stack']:continue
   frames=SPLIT.split(row['Stack'])
   if FRAMES['StateGameLoop'] not in frames:continue
   ns=int(Decimal(row['Weight (ms)'].replace(',',''))*1000000);count=int(row['Count'].replace(',',''))
   for name,frame in FRAMES.items():
    if frame in frames:result[name][0]+=ns;result[name][1]+=count
 return {k:dict(ns=result[k][0],samples=result[k][1],ms_per_tick=result[k][0]/1e6/TICKS) for k in FRAMES}
assert (WORK/'post-complete.txt').is_file()
captures=load(WORK/'capture-results.json');runs={};rows=[];settings={}
fields=['input_sha256','requested_ticks','resume_settings','precomputed','hyperpath','hyperpath_filter_mode','main_thread_affinity_mask','warm_precomputed','cold_precomputed','rebuild_precomputed','full_rebuild_precomputed','validation_only']
for name,label in ROUNDS:
 summary=load(OUT/name/'summary.json')
 for mode in ['legacy','hyperpath']:
  key=name+'/'+mode;meta=load(OUT/name/mode/'process.json');s=summary[mode]
  current={k:meta[k] for k in fields}
  if mode in settings:assert settings[mode]==current,key
  else:settings[mode]=current
  assert not s['unresolved_product_functions'] and s['payments_valid'] and s['ticks']==TICKS
  focus=cpu(OUT/name,mode);ft=tables(OUT/name/(mode+'-functions.csv'))
  for metric,frame in FRAMES.items():
   assert frame in ft,(key,frame)
   assert abs(float(ft[frame]['inclusive_ms_per_tick'])-focus[metric]['ms_per_tick'])<1e-9
  assert abs(focus['StateGameLoop']['ms_per_tick']-s['sampled_cpu_ms_per_tick']['simulation_stack'])<1e-9
  diag=captures[key]['diagnostics'];g=diag['generation_cause'];sw=diag['generation_sweeps']
  values={k:v['ms_per_tick'] for k,v in focus.items()}
  values.update(generation_packets=g['packets'],generation_us=g['microseconds'],generation_ms_per_tick=g['microseconds']/1000/TICKS,generation_us_per_packet=g['microseconds']/g['packets'] if g['packets'] else None,simulation_wall_ms_per_tick=s['simulation_wall_ms_per_tick'])
  values.update({'sweeps_'+k:sw[k] if sw else None for k in ['full','row_limited','skipped_packets']})
  runs[key]=dict(round=name,label=label,mode=mode,focus=focus,values=values,diagnostics=diag,conservation_checks=s['conservation_checks'])
  rows.append(dict(run=key,label=label,mode=mode,**values,route_samples=focus['RoutePassengerJourneys']['samples']))
comparison={};diff=[]
for mode in ['legacy','hyperpath']:
 comparison[mode]={}
 for metric in next(iter(runs.values()))['values']:
  groups={}
  for label in ['A','B']:
   selected=[r for r in runs.values() if r['mode']==mode and r['label']==label]
   vals=[r['values'][metric] for r in selected]
   groups[label]=dict(mean=statistics.mean(vals) if all(v is not None for v in vals) else None,minimum=min(vals) if all(v is not None for v in vals) else None,maximum=max(vals) if all(v is not None for v in vals) else None,values=vals,samples=sum(r['focus'][metric]['samples'] for r in selected) if metric in FRAMES else None)
  a=groups['A']['mean'];b=groups['B']['mean']
  comparison[mode][metric]=dict(**groups,delta=b-a if a is not None and b is not None else None,raw_percent=percent(a,b) if a is not None and b is not None else None)
 ts={name:tables(OUT/name/(mode+'-functions.csv')) for name,_ in ROUNDS}
 for fn in sorted(set().union(*(x.keys() for x in ts.values()))):
  row=dict(mode=mode,function=fn)
  for kind in ['inclusive','exclusive']:
   a=statistics.mean(float(ts[n].get(fn,{}).get(kind+'_ms_per_tick',0)) for n,l in ROUNDS if l=='A');b=statistics.mean(float(ts[n].get(fn,{}).get(kind+'_ms_per_tick',0)) for n,l in ROUNDS if l=='B')
   row.update({kind+'_A_mean_ms_per_tick':a,kind+'_B_mean_ms_per_tick':b,kind+'_delta_ms_per_tick':b-a,kind+'_percent':percent(a,b)})
  diff.append(row)
canonical={};cross={};validate={}
for label,names in [('A',['01-A1','04-A2']),('B',['02-B1','03-B2'])]:
 canonical[label]={}
 for mode in ['legacy','hyperpath']:
  a,b=[load(OUT/n/mode/'save-integrity.json') for n in names];assert len(a)==len(b)==24
  d={k:[a.get(k),b.get(k)] for k in sorted(a.keys()|b.keys()) if a.get(k)!=b.get(k)}
  canonical[label][mode]=dict(equal=not d,fields=24,differences=d,runs=names)
  assert not d,(label,mode,d)
 v=load(WORK/label/'validate-result.json');actual=load(WORK/label/'validate/save-integrity.json');normal=load(OUT/names[0]/'hyperpath/save-integrity.json')
 d={k:[actual.get(k),normal.get(k)] for k in sorted(actual.keys()|normal.keys()) if actual.get(k)!=normal.get(k)}
 validate[label]=dict(counters_passed=v['counters_passed'],validate_vs_normal_index_equal=not d,differences=d)
for mode in ['legacy','hyperpath']:
 a=load(OUT/'01-A1'/mode/'save-integrity.json');b=load(OUT/'02-B1'/mode/'save-integrity.json')
 d={k:[a.get(k),b.get(k)] for k in sorted(a.keys()|b.keys()) if a.get(k)!=b.get(k)}
 cross[mode]=dict(equal=not d,differences=d,expected_difference=True,acceptance_gate=False)
write_csv(OUT/'focus-runs.csv',rows);write_csv(OUT/'function-before-after.csv',diff)
dump(OUT/'focus.json',dict(runs=runs,comparison=comparison))
dump(OUT/'canonical-comparison.json',dict(within_version=canonical,cross_version=cross,validate_vs_index=validate))
dump(OUT/'comparison.json',dict(A_commit=load(WORK/'A/build-identity.json')['product_commit'],B_commit=load(WORK/'B/build-identity.json')['product_commit'],order=list(runs),ticks_each=TICKS,same_settings=settings,matched_clang_thinlto_conditions=True,untouched_correction_used=False,method='Two fresh processes per version/mode in ABBA; inclusive CPU counts a frame once per StateGameLoop stack. Counters describe changed workload; cross-version canonical is not a gate.',limitations='Two runs per version/mode; observed ranges are not confidence intervals. ABBA balances linear time drift, not all noise.',comparison=comparison))
(WORK/'analysis-complete.txt').write_text('Scheduler counters/function/canonical analysis complete\n',encoding='utf-8',newline='\n')
print(json.dumps(comparison,indent=2),flush=True)
