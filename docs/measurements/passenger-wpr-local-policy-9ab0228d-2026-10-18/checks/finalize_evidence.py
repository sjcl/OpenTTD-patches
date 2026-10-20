"""Validate canonical comparison coverage, cleanup, report links and final evidence bytes."""
from pathlib import Path
import hashlib
import json
import re

ROOT=Path(__file__).resolve().parents[1]
NAME='passenger-wpr-local-policy-9ab0228d-2026-10-18'
OUT=ROOT/'docs/measurements'/NAME
REPORT=ROOT/'docs'/(NAME+'.md')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
v=load(OUT/'validation.json');c=load(OUT/'canonical-comparison.json');d=load(OUT/'previous-results-cleanup.json')
assert v['product_commit']=='9ab0228db17178ef931de961305ebec544162f6a'
assert v['canonical_equality_checked'] and set(v['runs'])==set(c)=={'legacy','hyperpath'}
assert d['all_previous_targets_removed'] and len(d['targets'])==9
assert all(t['removed'] and not Path(t['path']).exists() for t in d['targets'])
for mode in c:
    assert c[mode]['fields']==24
    assert v['runs'][mode]['canonical_equal']==c[mode]['equal']
    assert v['runs'][mode]['exact_ticks']==2048 and v['runs'][mode]['kernel_window_valid']
    assert v['runs'][mode]['lost_events']==v['runs'][mode]['lost_buffers']==0
    assert v['runs'][mode]['raw_csv_hashes_match'] and v['runs'][mode]['payments_valid']
for link in re.findall(r'\]\(([^)]+)\)',REPORT.read_text(encoding='utf-8')):
    if '://' not in link:assert (REPORT.parent/link.split('#')[0]).exists(),link
(OUT/'README.md').write_text(f'# LocalPolicy 9ab0228d vs f9c4a85b\n\nSee the [report](../../{NAME}.md).\n\nBoth modes replay the same unchanged input for 2048 ticks. `canonical-comparison.json` compares all 24 endpoint canonical fields against the Git-tracked f9c4a85b results. `focus.json` and `function-before-after.csv` include raw and machine-adjusted inclusive/exclusive CPU ms/tick. `legacy/` and `hyperpath/` retain WPA raw CSV gzip files and exact-window metadata. `previous-results-cleanup.json` records deletion of previous untracked measurement/build folders and old external backups at the user request; old ETLs are no longer available. Only current artifacts remain outside Git. `checks/` retains the original preparation and verification helpers, including steps that ran before old directories were removed.\n',encoding='utf-8',newline='\n')
report=REPORT.read_bytes();assert b'\r\n' not in report
(OUT/'report-sha256.json').write_text(json.dumps(dict(path='../../'+REPORT.name,
    representation='Git blob: UTF-8 with LF; checkout may convert to CRLF',bytes=len(report),sha256=hashlib.sha256(report).hexdigest()),indent=2)+'\n')
manifest={}
for p in sorted(OUT.rglob('*')):
    if not p.is_file() or p.name=='evidence-sha256.json':continue
    assert p.suffix.lower() not in ['.sav','.exe','.pdb','.etl'],p
    assert p.stat().st_size<100_000_000,p
    manifest[p.relative_to(OUT).as_posix()]=dict(bytes=p.stat().st_size,sha256=sha(p))
(OUT/'evidence-sha256.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(dict(accepted_traces=2,canonical_comparisons=2,all_canonical_equal=all(x['equal'] for x in c.values()),
    previous_targets_deleted=9,files=len(manifest),bytes=sum(x['bytes'] for x in manifest.values())),indent=2))
