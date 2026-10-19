from pathlib import Path
import json,re
BASE=Path(__file__).resolve().parent
runs=[];canonical={}
for mode in ('baseline','pgo'):
    name=f'plain-{mode}-1';directory=BASE/'runs'/name
    r=json.loads((directory/'process.json').read_text())
    clock=r['actual-ticks.json']
    assert r['exit_code']==0 and r['simulation_timing_valid'] and r['warm_complete'] and r['save-integrity.json']['payments_valid']
    assert [s[0] for s in clock['samples']]==list(range(3197276449,3197277473))
    text=(directory/'on-1-console.txt').read_text(encoding='utf-8',errors='replace')
    warm=re.search(r'Precomputed warm: ready=true; elapsed_us=(\d+); cpu_us=(\d+)',text)
    assert warm
    runs.append(dict(name=name,mode=mode,ticks=1024,start_tick=clock['start_tick'],end_tick=clock['end_tick'],
        world_cpu_ms_per_tick=sum(s[1] for s in clock['samples'])/1000/1024,
        build_cpu_seconds=int(warm[2])/1e6,build_wall_seconds=int(warm[1])/1e6))
    canonical[name]=r['save-integrity.json']
reference=canonical[runs[0]['name']]
mismatches={name:{key:[reference.get(key),value.get(key)] for key in set(reference)|set(value) if reference.get(key)!=value.get(key)} for name,value in canonical.items()}
assert not any(mismatches.values())
out=dict(method='GetThreadTimes CPU counters in the copied StateGameLoop scope; coarse per-tick accounting; not WPR sampled CPU',runs=runs,canonical=canonical,mismatches=mismatches)
(BASE/'summary-plain.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(runs,indent=2))
