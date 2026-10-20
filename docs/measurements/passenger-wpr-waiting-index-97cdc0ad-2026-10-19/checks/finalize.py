"""Inventory the final report/evidence bytes and validate references."""
from pathlib import Path
import hashlib
import json
import re
ROOT=Path(__file__).resolve().parents[1]
NAME='passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
OUT=ROOT/'docs/measurements'/NAME
REPORT=ROOT/'docs'/(NAME+'.md')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
v=load(OUT/'validation.json');c=load(OUT/'canonical-comparison.json')
assert v['product_commit']=='97cdc0ad4c9493daea0db6079e90a03924d96b56'
assert v['canonical_equality_checked'] and set(v['runs'])==set(c)=={'legacy','hyperpath'}
for mode in c:
    assert c[mode]['fields']==24 and v['runs'][mode]['canonical_equal']==c[mode]['equal']
    assert v['runs'][mode]['exact_ticks']==2048 and v['runs'][mode]['kernel_window_valid']
    assert v['runs'][mode]['lost_events']==v['runs'][mode]['lost_buffers']==0
    assert v['runs'][mode]['raw_csv_hashes_match'] and v['runs'][mode]['payments_valid']
for link in re.findall(r'\]\(([^)]+)\)',REPORT.read_text(encoding='utf-8')):
    if '://' not in link:assert (REPORT.parent/link.split('#')[0]).exists(),link
(OUT/'README.md').write_text(f'# Waiting-index WPR: 97cdc0ad vs 9ab0228d\n\nSee the [report](../../{NAME}.md).\n\nBoth modes replay the same unchanged input for 2048 ticks. All 24 canonical fields are compared independently by mode. `focus.json` covers the six requested inclusive functions; `function-before-after.csv` also contains exclusive values and machine-adjusted estimates. `legacy/` and `hyperpath/` retain raw WPA CSV gzip files and exact-window metadata. Recording modes match the baseline by mode: Legacy Memory, Hyperpath File. `recording-backup.json` locates the ETL and matching executable/PDB; `input-backup.json` references the identical retained frozen input. `checks/` contains build/capture/analysis scripts and copied measurement TUs. The helpers document the original workspace paths; restore those inputs when reproducing.\n',encoding='utf-8',newline='\n')
report=REPORT.read_bytes();assert b'\r\n' not in report
(OUT/'report-sha256.json').write_text(json.dumps(dict(path='../../'+REPORT.name,representation='UTF-8 LF Git blob',bytes=len(report),sha256=hashlib.sha256(report).hexdigest()),indent=2)+'\n')
manifest={}
for p in sorted(OUT.rglob('*')):
    if not p.is_file() or p.name=='evidence-sha256.json':continue
    assert p.suffix.lower() not in ['.sav','.exe','.pdb','.etl'] and p.stat().st_size<100_000_000,p
    manifest[p.relative_to(OUT).as_posix()]=dict(bytes=p.stat().st_size,sha256=sha(p))
(OUT/'evidence-sha256.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(dict(accepted_traces=2,all_canonical_equal=all(x['equal'] for x in c.values()),files=len(manifest),bytes=sum(x['bytes'] for x in manifest.values())),indent=2))
