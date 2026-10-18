"""Inventory the final two-capture evidence and validate all report links."""
from pathlib import Path
import hashlib
import json
import re

ROOT=Path(__file__).resolve().parents[1]
NAME='passenger-wpr-generation-8ff4834d-2026-10-18'
OUT=ROOT/'docs/measurements'/NAME
REPORT=ROOT/'docs'/(NAME+'.md')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
v=load(OUT/'validation.json');c=load(OUT/'comparison.json')
assert v['product_commit']=='8ff4834dba34f6ae18ea2068a542ecf47a4ac5e5'
assert set(v['runs'])==set(c)=={'legacy','hyperpath'}
for mode in c:
    assert not c[mode]['canonical_equality_checked']
    assert v['runs'][mode]['exact_ticks']==2048 and v['runs'][mode]['kernel_window_valid']
    assert v['runs'][mode]['lost_events']==v['runs'][mode]['lost_buffers']==0
    assert v['runs'][mode]['raw_csv_hashes_match'] and v['runs'][mode]['payments_valid']
for link in re.findall(r'\]\(([^)]+)\)',REPORT.read_text(encoding='utf-8')):
    if '://' not in link:assert (REPORT.parent/link.split('#')[0]).exists(),link
(OUT/'README.md').write_text(f'# Ordinary 8ff4834d captures\n\nSee the [report](../../{NAME}.md).\n\n`comparison.json` records generation counts, diagnostic wall time, waiting-event inclusive CPU time and endpoint overdue values. Endpoint canonical equality is deliberately not checked. `function-before-after.csv` compares raw inclusive/exclusive CPU ms/tick and includes machine-adjusted values for reference. `legacy/` and `hyperpath/` retain WPA raw CSV gzip files and exact-window metadata. `checks/` contains build, conversion, validation and reproduction helpers.\n',encoding='utf-8',newline='\n')
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
print(json.dumps(dict(accepted_traces=2,canonical_equality_checked=False,files=len(manifest),bytes=sum(x['bytes'] for x in manifest.values())),indent=2))
