"""Measure three disjoint stack regions without adding overlapping inclusive costs."""
from pathlib import Path
from decimal import Decimal
import csv
import gzip
import json
import re

ROOT = Path(__file__).resolve().parent.parent
EVIDENCE = ROOT/'docs/measurements/passenger-routing-steady-pair-8462e6cd-2026-10-18'
roots = ['openttd.exe!PassengerServiceDatabase::Tick', 'openttd.exe!RunPassengerWaitingEvents', 'openttd.exe!LoadUnloadVehicle']
result = {}
summary = json.loads((EVIDENCE/'summary.json').read_text())
for mode in ['legacy', 'hyperpath']:
    tid = summary[mode]['main_thread_id']
    union_ns, samples, max_roots = 0, 0, 0
    components = {root:0 for root in roots}
    with gzip.open(EVIDENCE/mode/'CPU_Usage_(Sampled)_Stacks.csv.gz', 'rt', encoding='utf-8-sig', newline='') as stream:
        for row in csv.DictReader(stream):
            if not row['Thread ID'] or int(row['Thread ID']) != tid or not row['Stack']:
                continue
            frames = set(re.split(r'/(?=[^/!]+\.(?:exe|dll|sys)!)',row['Stack'], flags=re.I))
            if 'openttd.exe!StateGameLoop' not in frames:
                continue
            matches = [root for root in roots if root in frames]
            max_roots = max(max_roots, len(matches))
            if not matches:
                continue
            weight = int(Decimal(row['Weight (ms)'].replace(',',''))*1000000)
            union_ns += weight
            samples += int(row['Count'].replace(',',''))
            for root in matches:
                components[root] += weight
    assert max_roots == 1
    assert sum(components.values()) == union_ns
    cpu = union_ns/1000000/2048
    result[mode] = dict(union_ns=union_ns, union_samples=samples, maximum_roots_in_one_stack=max_roots,
        cpu_ms_per_tick=cpu, components_ms_per_tick={root:value/1000000/2048 for root,value in components.items()},
        other_state_game_loop_cpu_ms_per_tick=summary[mode]['sampled_cpu_ms_per_tick']['simulation_stack']-cpu)
result['delta_union_cpu_ms_per_tick'] = result['hyperpath']['cpu_ms_per_tick']-result['legacy']['cpu_ms_per_tick']
result['delta_other_state_game_loop_cpu_ms_per_tick'] = result['hyperpath']['other_state_game_loop_cpu_ms_per_tick']-result['legacy']['other_state_game_loop_cpu_ms_per_tick']
result['method'] = 'Main-thread StateGameLoop stacks containing any of the three named roots. Actual traces show at most one root per stack; verify the union instead of summing nested inclusive costs. LoadUnloadVehicle includes all cargo.'
(EVIDENCE/'component-union.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
