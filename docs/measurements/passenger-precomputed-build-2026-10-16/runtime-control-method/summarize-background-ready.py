"""Summarize balanced actual-image controls without altering any tick sample."""
import hashlib,json,math,pathlib,re,statistics
root=pathlib.Path(__file__).resolve().parent
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def distribution(values):
 values=sorted(values)
 return dict(count=len(values),mean=statistics.mean(values),p95=values[math.ceil(.95*len(values))-1],
  p99=values[math.ceil(.99*len(values))-1],maximum=max(values))
result={}
for cohort in ['background-ready','background-ready-retained']:
 if not (root/('runtime-'+cohort+'-summary.json')).exists():continue
 runs={};pooled={version:dict(simulation=[],journey=[]) for version in ['old','new']}
 for label in ['old-a','new-a','new-b','old-b']:
  directory=root/('runtime-'+cohort+'-'+label)
  process=json.loads((directory/'process.json').read_text())
  host=json.loads((directory/'host-observation.json').read_text())
  ticks=json.loads((directory/'actual-ticks.json').read_text())['samples']
  rows=[r for r in host['samples'] if r['stage']=='simulation']
  assert len(ticks)==2048 and process['simulation_timing_valid']
  ready=process['background_ready'];assert ready['completed'] and ready['tick_before']==ready['tick_after']
  version=label.split('-')[0]
  for field,index in [('simulation',1),('journey',2)]:pooled[version][field].extend(t[index] for t in ticks)
  first,last=rows[0],rows[-1]
  main=(last['main_cpu_100ns']-first['main_cpu_100ns'])/1e7
  whole=(last['process_cpu_100ns']-first['process_cpu_100ns'])/1e7
  counters={}
  for name in host['counter_paths']:
   values=[r['counters'][name]['value'] for r in rows if r['counters'][name]['value'] is not None]
   counters[name]=distribution(values) if values else None
  runs[label]=dict(binary_sha256=process['binary_sha256'],input_sha256=process['input_sha256'],
   process_source_sha256=sha(directory/'process.json'),tick_source_sha256=sha(directory/'actual-ticks.json'),
   host_source_sha256=sha(directory/'host-observation.json'),background_ready=ready,
   warm_us=int(re.search(r'Precomputed warm: ready=true; elapsed_us=(\d+)',(directory/'on-1-console.txt').read_text()).group(1)),
   simulation_us=distribution([t[1] for t in ticks]),journey_us=distribution([t[2] for t in ticks]),
   host_counters=counters,monitor_interval=dict(samples=len(rows),elapsed_seconds=last['elapsed_seconds']-first['elapsed_seconds'],
    main_cpu_seconds=main,other_process_thread_cpu_seconds=whole-main),
   observation_cost_us=distribution([r['observation_cost_us'] for r in rows]),result_sha256=process['result_sha256'])
 aggregates={v:{f:distribution(values) for f,values in fields.items()} for v,fields in pooled.items()}
 change={field:{metric:(aggregates['new'][field][metric]/aggregates['old'][field][metric]-1)*100
  for metric in ['mean','p95','p99']} for field in ['simulation','journey']}
 result[cohort]=dict(order=['old-a','new-a','new-b','old-b'],runs=runs,pooled_us=aggregates,new_vs_old_percent=change)
output=dict(cohorts=result,note='All 2048 raw ticks in every run included, with no correction or exclusions. Pooled percentiles use 4096 ticks per version. External monitor intervals cover only the interior of the observed simulation stage, not exact StateGameLoop boundaries; non-main CPU includes every other game thread. Background-job readiness is a measurement-only barrier while simulation ticks remain frozen; no early publication is performed. Other applications remain active, so small differences are not evidence of a universal speedup.')
(root/'runtime-background-ready-analysis.json').write_text(json.dumps(output,indent=2)+'\n')
for name,cohort in result.items():
 print(name,cohort['pooled_us'],cohort['new_vs_old_percent'])
