"""Summarize requested counters and CPU samples; previous inputs are reference only."""
from pathlib import Path
import csv
import hashlib
import json
import re

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-generation-f9c4a85b-2026-10-18'
OUT=ROOT/'docs/measurements'/NAME
PREVIOUS=ROOT/'docs/measurements/passenger-wpr-generation-8ff4834d-2026-10-18'
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def fields(s):return {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',s)}
def diagnostic(directory):
    text=(directory/'on-1-console.txt').read_text(encoding='utf-8-sig')
    events=[s for s in text.splitlines() if s.startswith('Passenger events:')]
    generation=re.findall(r'Passenger scheduler cause: cause=generation; packets=(\d+); microseconds=(\d+)',text)
    unchanged=re.findall(r'Passenger unchanged generation: packets=(\d+)',text)
    assert events and generation and unchanged
    packets,microseconds=map(int,generation[-1])
    return dict(events=fields(events[-1]),generation=dict(packets=packets,microseconds=microseconds,
        packets_per_tick=packets/2048,diagnostic_wall_ms_per_tick=microseconds/2048/1000),
        unchanged_generation_packets=int(unchanged[-1]))
inputs=load(WORK/'input-validation.json');previous=load(PREVIOUS/'comparison.json')
result=dict(product_commit='f9c4a85b721073bbf86747f4a4cec7ac5ccd2f89',input_sha256=inputs['source_sha256'],
    same_input_between_modes=True,input_changed_since_previous=True,previous_is_reference_only=True,
    canonical_equality_checked=False,runs={})
for mode in ['legacy','hyperpath']:
    meta=load(WORK/mode/'process.json');ticks=meta['actual-ticks.json']
    assert meta['input_sha256']==inputs['source_sha256']
    assert ticks['start_tick']==inputs['start_tick'] and ticks['end_tick']==inputs['target_end_tick'] and len(ticks['samples'])==2048
    current=diagnostic(WORK/mode);initial=diagnostic(WORK/('validation-'+mode))
    with (OUT/(mode+'-functions.csv')).open(newline='',encoding='utf-8-sig') as f:
        rows=[r for r in csv.DictReader(f) if r['scope']=='simulation_stack' and r['function']=='openttd.exe!RunPassengerWaitingEvents']
    assert len(rows)==1,rows
    row=rows[0]
    current['RunPassengerWaitingEvents']={key:float(row[key]) for key in ['inclusive_ms_per_tick','exclusive_ms_per_tick']}
    current['RunPassengerWaitingEvents'].update({key:int(row[key]) for key in ['inclusive_samples','exclusive_samples']})
    result['runs'][mode]=dict(current=current,initial_zero_tick_events=initial['events'],
        overdue_cleared_at_end=current['events']['overdue']==0 and current['events']['oldest_ticks']==0,
        oldest_ticks_zero_at_end=current['events']['oldest_ticks']==0,
        previous_8ff4834d_reference=previous[mode]['current'],
        previous_input_sha256=previous[mode]['input_sha256'],input_matches_previous=False)
    assert previous[mode]['input_sha256']!=inputs['source_sha256']
    print(mode,json.dumps(result['runs'][mode],ensure_ascii=False),flush=True)
result['reference_files']={str((PREVIOUS/'comparison.json').relative_to(ROOT)):sha(PREVIOUS/'comparison.json')}
(OUT/'metrics.json').write_text(json.dumps(result,indent=2)+'\n')
