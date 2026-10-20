"""Compare only the new input's matched per-mode old/new captures."""
from pathlib import Path
import csv
import json
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent/'rebuild'
EVIDENCE=ROOT/'docs/measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild'

def load(p): return json.loads(p.read_text(encoding='utf-8'))
def dump(p,d): p.write_text(json.dumps(d,indent=2)+'\n',encoding='utf-8')
canonical=load(WORK/'canonical-comparison.json')
assert set(canonical)=={'legacy','hyperpath'} and all(d['equal'] and d['fields']==24 for d in canonical.values())
for mode in ['legacy','hyperpath']:
    old=load(EVIDENCE/'baseline'/mode/'save-integrity.json');new=load(EVIDENCE/'candidate'/mode/'save-integrity.json')
    assert old==new and len(old)==24
    oldmeta=load(EVIDENCE/'baseline'/mode/'process.json');newmeta=load(EVIDENCE/'candidate'/mode/'process.json')
    for key in ['input_sha256','language_sha256','requested_ticks','resume_settings','cold_precomputed','warm_precomputed',
                'precomputed','hyperpath','hyperpath_filter_mode','main_thread_affinity_mask','rebuild_precomputed']:
        assert oldmeta[key]==newmeta[key],(mode,key)
    for version in ['baseline','candidate']:
        audit=load(WORK/version/mode/'window-audit.json');assert audit['valid']
with (EVIDENCE/'control.json').open('wb') as out:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_control.py'),str(EVIDENCE/'baseline'),str(EVIDENCE/'candidate')],cwd=ROOT,stdout=out,check=True)
control=load(EVIDENCE/'control.json'); rows=[]
for mode in ['legacy','hyperpath']:
    tables={}
    for version in ['baseline','candidate']:
        with (EVIDENCE/version/(mode+'-functions.csv')).open(newline='',encoding='utf-8') as f:
            tables[version]={r['function']:r for r in csv.DictReader(f) if r['scope']=='simulation_stack'}
    factor=control[mode]['machine_factor']
    for function in sorted(tables['baseline'].keys()|tables['candidate'].keys()):
        before=tables['baseline'].get(function,{});after=tables['candidate'].get(function,{})
        r=dict(mode=mode,function=function,machine_factor=factor)
        for kind in ['inclusive','exclusive']:
            old=float(before.get(kind+'_ms_per_tick',0));new=float(after.get(kind+'_ms_per_tick',0));normalized=new/factor
            r.update({kind+'_old_ms_per_tick':old,kind+'_new_ms_per_tick':new,kind+'_raw_delta':new-old,
                kind+'_raw_percent':100*(new/old-1) if old else None,kind+'_corrected_ms_per_tick':normalized,
                kind+'_corrected_delta':normalized-old,kind+'_corrected_percent':100*(normalized/old-1) if old else None})
        rows.append(r)
rows.sort(key=lambda r:(r['mode'],-abs(r['inclusive_corrected_delta'])))
with (EVIDENCE/'function-before-after.csv').open('w',newline='',encoding='utf-8') as f:
    writer=csv.DictWriter(f,list(rows[0]));writer.writeheader();writer.writerows(rows)
names=['PassengerPrecomputedTable::StepHyperpath','PassengerServiceDatabase::TickPrecomputed','PassengerServiceDatabase::Tick',
       'PassengerPrecomputedTable::SetSearchLabel','PassengerPrecomputedTable::RelaxIncoming','StateGameLoop']
focus=[r for r in rows if r['function'].removeprefix('openttd.exe!') in names]
for mode,name in [('hyperpath','PassengerPrecomputedTable::StepHyperpath'),
                  ('hyperpath','PassengerServiceDatabase::TickPrecomputed'),
                  ('legacy','PassengerServiceDatabase::TickPrecomputed')]:
    r=next(r for r in focus if r['mode']==mode and r['function'].removeprefix('openttd.exe!')==name)
    assert r['inclusive_old_ms_per_tick']>0 and r['inclusive_new_ms_per_tick']>0, (mode,name)
dump(EVIDENCE/'focus.json',dict(canonical=canonical,control=control,functions=focus,
    previous_input_comparable=False,old_historical_hyperpath_reference_ms_per_tick=dict(StepHyperpath=2.954580420,TickPrecomputed=3.010732959)))
print(json.dumps(dict(control=control,functions=focus),indent=2))
