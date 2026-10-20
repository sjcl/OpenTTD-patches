"""Compare counts and timings to the previous ordinary captures, without canonical equality."""
from pathlib import Path
import csv
import gzip
import hashlib
import json
import re
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-generation-8ff4834d-2026-10-18'
OUT=ROOT/'docs/measurements'/NAME
OLD=ROOT/'docs/measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/candidate'
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def fields(line):return {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}
def diagnostics(text):
    events=[s for s in text.splitlines() if s.startswith('Passenger events:')]
    generation=re.findall(r'Passenger scheduler cause: cause=generation; packets=(\d+); microseconds=(\d+)',text)
    unchanged=re.findall(r'Passenger unchanged generation: packets=(\d+)',text)
    assert events and generation and unchanged
    count,microseconds=map(int,generation[-1])
    return dict(events=fields(events[-1]),generation=dict(packets=count,microseconds=microseconds,
        packets_per_tick=count/2048,diagnostic_wall_ms_per_tick=microseconds/2048/1000),unchanged_generation_packets=int(unchanged[-1]))
def functions(p):
    with p.open(newline='',encoding='utf-8-sig') as f:return {r['function']:r for r in csv.DictReader(f) if r['scope']=='simulation_stack'}
def percent(old,new):return 100*(new/old-1) if old else None
with (OUT/'machine-control.json').open('wb') as stream:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_control.py'),str(OLD),str(OUT)],cwd=ROOT,stdout=stream,check=True)
control=load(OUT/'machine-control.json');comparison={};rows=[]
conversion=load(WORK/'input-conversion-validation.json')
for mode in ['legacy','hyperpath']:
    old_meta=load(OLD/mode/'process.json');new_meta=load(OUT/mode/'process.json')
    assert old_meta['input_sha256']==conversion[mode]['source_input_sha256']
    assert new_meta['input_sha256']==conversion[mode]['input_sha256']
    assert conversion[mode]['canonical_fields_equal']==22 and conversion[mode]['consolidated_database_wire_identical']
    for key in ['requested_ticks','resume_settings','warm_precomputed','cold_precomputed','rebuild_precomputed',
                'full_rebuild_precomputed','precomputed','hyperpath','hyperpath_filter_mode','main_thread_affinity_mask']:
        assert old_meta[key]==new_meta[key],(mode,key)
    for meta in [old_meta,new_meta]:
        clock=meta['actual-ticks.json']
        assert clock['start_tick']==13647872 and clock['end_tick']==13649920 and len(clock['samples'])==2048
    old=diagnostics(gzip.decompress((OLD/mode/'on-1-console.txt.gz').read_bytes()).decode('utf-8-sig'))
    new=diagnostics((WORK/mode/'on-1-console.txt').read_text(encoding='utf-8-sig'))
    a=functions(OLD/(mode+'-functions.csv'));b=functions(OUT/(mode+'-functions.csv'))
    factor=control[mode]['machine_factor']
    for function in sorted(a.keys()|b.keys()):
        before=a.get(function,{});after=b.get(function,{})
        r=dict(mode=mode,function=function,machine_factor_reference=factor)
        for kind in ['inclusive','exclusive']:
            x=float(before.get(kind+'_ms_per_tick',0));y=float(after.get(kind+'_ms_per_tick',0))
            r.update({kind+'_old_ms_per_tick':x,kind+'_new_ms_per_tick':y,kind+'_delta_ms_per_tick':y-x,
                kind+'_percent':percent(x,y),kind+'_machine_adjusted_reference_ms_per_tick':y/factor})
        rows.append(r)
    name='openttd.exe!RunPassengerWaitingEvents'
    for metrics,table in [(old,a),(new,b)]:
        metrics['RunPassengerWaitingEvents']=dict(inclusive_cpu_ms_per_tick=float(table.get(name,{}).get('inclusive_ms_per_tick',0)),
            inclusive_samples=int(table.get(name,{}).get('inclusive_samples',0)))
    comparison[mode]=dict(previous=old,current=new,input_sha256=new_meta['input_sha256'],
        source_input_sha256=old_meta['input_sha256'],input_format_conversion_validated=True,
        generation_packet_percent=percent(old['generation']['packets'],new['generation']['packets']),
        generation_diagnostic_time_percent=percent(old['generation']['microseconds'],new['generation']['microseconds']),
        waiting_inclusive_cpu_percent=percent(old['RunPassengerWaitingEvents']['inclusive_cpu_ms_per_tick'],new['RunPassengerWaitingEvents']['inclusive_cpu_ms_per_tick']),
        overdue_cleared_at_end=new['events']['overdue']==0 and new['events']['oldest_ticks']==0,
        canonical_equality_checked=False,machine_adjustment_is_reference_only=True,
        reference_files={p.relative_to(ROOT).as_posix():sha(p) for p in [OLD/(mode+'-functions.csv'),OLD/mode/'process.json',OLD/mode/'on-1-console.txt.gz']})
with (OUT/'function-before-after.csv').open('w',newline='',encoding='utf-8') as f:
    writer=csv.DictWriter(f,list(rows[0]));writer.writeheader();writer.writerows(rows)
(OUT/'comparison.json').write_text(json.dumps(comparison,indent=2)+'\n')
print(json.dumps(comparison,indent=2),flush=True)
