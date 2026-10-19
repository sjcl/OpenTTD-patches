"""Inventory the final report/evidence bytes and validate references."""
from pathlib import Path
import hashlib
import json
import re
ROOT=Path(__file__).resolve().parents[1]
NAME='passenger-wpr-coach-memo-0b8a21c0-2026-10-19'
OUT=ROOT/'docs/measurements'/NAME
REPORT=ROOT/'docs'/(NAME+'.md')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
v=load(OUT/'validation.json');c=load(OUT/'canonical-comparison.json')
assert v['product_commit']=='0b8a21c0f3dc9a24ca0d9d3581c19fb9c207e684'
assert v['canonical_equality_checked'] and set(v['runs'])==set(c)=={'legacy','hyperpath'}
validate=load(OUT/'validate-result.json')
assert v['validate']==validate and validate['exact_ticks']==2048
assert validate['canonical']['fields']==24 and validate['excluded_from_performance_comparison']
assert validate['hyperpath_filter_mode']=='validate'
for mode in c:
    assert c[mode]['fields']==24 and v['runs'][mode]['canonical_equal']==c[mode]['equal']
    assert v['runs'][mode]['exact_ticks']==2048 and v['runs'][mode]['kernel_window_valid']
    assert v['runs'][mode]['lost_events']==v['runs'][mode]['lost_buffers']==0
    assert v['runs'][mode]['raw_csv_hashes_match'] and v['runs'][mode]['payments_valid']
for link in re.findall(r'\]\(([^)]+)\)',REPORT.read_text(encoding='utf-8')):
    if '://' not in link:assert (REPORT.parent/link.split('#')[0]).exists(),link
(OUT/'README.md').write_text(f'# Coach-memo WPR: 0b8a21c0 vs 97cdc0ad\n\nSee the [report](../../{NAME}.md).\n\nBoth modes replay the same fixed input for 2048 ticks. The 24 canonical fields are compared independently by mode. Raw WPA CSV gzip files, function tables, sampled counts and machine-adjusted estimates are retained. Both accepted traces use File mode with larger collector buffers after missing markers and dropped events in the original attempts. Hyperpath retains the baseline CPU providers/stacks and File mode, while Legacy also changes from Memory to File. Both time comparisons include collector-buffer differences; see recorder-settings.json. The separate validate run checks memo reuse and is excluded from performance measurements. Recording and input backup manifests locate the raw artifacts. Scripts in checks document the original workspace paths.\n',encoding='utf-8',newline='\n')
report=REPORT.read_bytes();assert b'\r\n' not in report
(OUT/'report-sha256.json').write_text(json.dumps(dict(path='../../'+REPORT.name,representation='UTF-8 LF Git blob',bytes=len(report),sha256=hashlib.sha256(report).hexdigest()),indent=2)+'\n')
manifest={}
for p in sorted(OUT.rglob('*')):
    if not p.is_file() or p.name=='evidence-sha256.json':continue
    assert p.suffix.lower() not in ['.sav','.exe','.pdb','.etl'] and p.stat().st_size<100_000_000,p
    manifest[p.relative_to(OUT).as_posix()]=dict(bytes=p.stat().st_size,sha256=sha(p))
(OUT/'evidence-sha256.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(dict(accepted_traces=2,all_canonical_equal=all(x['equal'] for x in c.values()),validate_memo=validate['memo'],validate_canonical_equal=validate['canonical']['equal'],files=len(manifest),bytes=sum(x['bytes'] for x in manifest.values())),indent=2))
