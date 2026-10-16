"""Descriptive host intervals; retain all actual tick samples without correction."""
import hashlib,json,math,pathlib,statistics
root=pathlib.Path(__file__).resolve().parent
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()

def distribution(values):
    values=sorted(values)
    return dict(count=len(values),mean=statistics.mean(values),p95=values[math.ceil(.95*len(values))-1],
                p99=values[math.ceil(.99*len(values))-1],maximum=max(values))

result={}
for mode in ['legacy-c','integrated-c','integrated-d','legacy-d']:
    directory=root/('runtime-host-observed-'+mode)
    process=json.loads((directory/'process.json').read_text())
    host=json.loads((directory/'host-observation.json').read_text())
    ticks=json.loads((directory/'actual-ticks.json').read_text())['samples']
    rows=[row for row in host['samples'] if row['stage']=='simulation']
    counters={}
    for name in host['counter_paths']:
        values=[row['counters'][name]['value'] for row in rows if row['counters'][name]['value'] is not None]
        counters[name]=dict(count=len(values),mean=statistics.mean(values),minimum=min(values),maximum=max(values)) if values else None
    first,last=rows[0],rows[-1]
    main=(last['main_cpu_100ns']-first['main_cpu_100ns'])/1e7
    whole=(last['process_cpu_100ns']-first['process_cpu_100ns'])/1e7
    main_id=process['main_thread']['thread_id']
    workers=host['worker_affinity_snapshot']
    result[mode]=dict(binary_sha256=process['binary_sha256'],input_sha256=process['input_sha256'],
        tick_source_sha256=sha(directory/'actual-ticks.json'),host_source_sha256=sha(directory/'host-observation.json'),
        simulation_us=distribution([tick[1] for tick in ticks]),journey_us=distribution([tick[2] for tick in ticks]),
        host_counters=counters,monitor_interval=dict(samples=len(rows),elapsed_seconds=last['elapsed_seconds']-first['elapsed_seconds'],
            main_cpu_seconds=main,other_process_thread_cpu_seconds=whole-main),
        observation_cost_us=distribution([row['observation_cost_us'] for row in rows]),
        actual_worker_affinity_matches_process=all(row['mask']==process['main_thread']['process_affinity'] for row in workers if row['thread_id']!=main_id),
        linkgraph_threads_in_preparation=[row for row in workers if row.get('description')=='ottd:linkgraph'],
        result_sha256=process['result_sha256'])
output=dict(runs=result,note='No tick samples excluded or normalized. OS monitor intervals cover the interior of the externally observed simulation phase, not exact StateGameLoop CPU boundaries. Non-main CPU includes every other thread of this game process, not only linkgraph. Global CPU includes unrelated applications. Provider frequency/performance data are not assumed to be exact hardware instruction cycles.')
(root/'runtime-host-observed-analysis.json').write_text(json.dumps(output,indent=2)+'\n')
for name,row in result.items():
    print(name,dict(simulation_mean_ms=row['simulation_us']['mean']/1000,p95_ms=row['simulation_us']['p95']/1000,p99_ms=row['simulation_us']['p99']/1000,
        monitor=row['monitor_interval'],cpu0_performance=row['host_counters']['cpu0_performance_percent']['mean'],
        global_cpu=row['host_counters']['total_time_percent']['mean'],maximum_observation_us=row['observation_cost_us']['maximum']))
