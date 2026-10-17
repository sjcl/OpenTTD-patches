from pathlib import Path
from decimal import Decimal
import csv,gzip,json,re

root=Path('C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches')
e=root/'docs/measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17'
control=json.loads((e/'control.json').read_text(encoding='utf-8'))
functions=['openttd.exe!StationCargoList::'+name for name in ['RefreshHyperpathLookup','IndexHyperpathGroup','UnindexHyperpathGroup']]+['openttd.exe!PassengerServiceDatabase::HyperpathLookupCandidates']
result={}
for mode in ['legacy','hyperpath']:
    meta=json.loads((e/mode/'process.json').read_text(encoding='utf-8'))
    tid=str(meta['main_thread']['thread_id'])
    total_ns=count=0
    with gzip.open(e/mode/'CPU_Usage_(Sampled)_Stacks.csv.gz','rt',encoding='utf-8-sig',newline='') as stream:
        for row in csv.DictReader(stream):
            if row['Thread ID']!=tid or not row['Stack']:continue
            frames=re.split(r'/(?=[^/!]+\.(?:exe|dll|sys)!)',row['Stack'],flags=re.I)
            if 'openttd.exe!StateGameLoop' not in frames:continue
            if any(name in frames for name in functions):
                total_ns+=int(Decimal(row['Weight (ms)'].replace(',',''))*1000000)
                count+=int(row['Count'].replace(',',''))
    with (e/(mode+'-functions.csv')).open(encoding='utf-8',newline='') as stream:
        rows=[row for row in csv.DictReader(stream) if row['scope']=='simulation_stack' and row['function'] in functions]
    raw=total_ns/1000000/2048
    result[mode]=dict(scope='Main-thread StateGameLoop stacks containing any named maintenance function; union, not inclusive sum',
        functions=functions,samples=count,raw_cpu_ms_per_tick=raw,corrected_cpu_ms_per_tick=raw/control[mode]['machine_factor'],function_rows=rows)
(e/'maintenance.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
