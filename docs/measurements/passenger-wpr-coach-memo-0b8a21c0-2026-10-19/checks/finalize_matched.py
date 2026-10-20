"""Inventory the four-trace report and verify canonical coverage and links."""
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
v=load(OUT/'validation.json');assert v['accepted_traces']==4 and v['matched_kernel_cpu_profile']
assert v['product_commit']=='0b8a21c0f3dc9a24ca0d9d3581c19fb9c207e684'
for group in ['runs','control_runs']:
    assert set(v[group])=={'legacy','hyperpath'}
    for row in v[group].values():
        assert row['canonical_fields']==24 and row['exact_ticks']==2048 and row['kernel_window_valid']
        assert row['lost_events']==row['lost_buffers']==0 and row['raw_csv_hashes_match'] and row['payments_valid']
assert v['validate']==load(OUT/'validate-result.json') and v['validate']['canonical']['fields']==24
assert v['memo_validation_passed']==(v['validate']['reuse_observed'] and v['validate']['memo']['violations']==0)
for link in re.findall(r'\]\(([^)]+)\)',REPORT.read_text(encoding='utf-8')):
    if '://' not in link:assert (REPORT.parent/link.split('#')[0]).exists(),link
(OUT/'README.md').write_text(f'# Coach memo WPR: 0b8a21c0 versus matched 97cdc0ad controls\n\nSee the [report](../../{NAME}.md).\n\nMemo validation passed: {v["memo_validation_passed"]}. The memo line reports {v["validate"]["memo"]["validated"]} validations and {v["validate"]["memo"]["violations"]} violations despite matching endpoint canonical states.\n\nFour accepted traces use the same kernel CPU-only File profile after noisy full profiles lost events. `control-97cdc0ad/` contains new controls using the exact historical binary/PDB. Every run checks the original 24-field canonical state. `historical-focus.json` retains previous full-profile reference values; these are separate from the matched primary comparison. `focus.json`, raw WPA CSV gzip files, vector reallocation stacks, and the independent memo validate result are included. Raw ETL/binary backups are located by `recording-backup.json`; identical input archive by `input-backup.json`. Scripts preserve the original workspace paths and rejected attempts for audit.\n',encoding='utf-8',newline='\n')
report=REPORT.read_bytes();assert b'\r\n' not in report
(OUT/'report-sha256.json').write_text(json.dumps(dict(path='../../'+REPORT.name,representation='UTF-8 LF Git blob',bytes=len(report),sha256=hashlib.sha256(report).hexdigest()),indent=2)+'\n')
manifest={}
for p in sorted(OUT.rglob('*')):
    if not p.is_file() or p.name=='evidence-sha256.json':continue
    assert p.suffix.lower() not in ['.sav','.exe','.pdb','.etl'] and p.stat().st_size<100_000_000,p
    manifest[p.relative_to(OUT).as_posix()]=dict(bytes=p.stat().st_size,sha256=sha(p))
(OUT/'evidence-sha256.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(dict(accepted_traces=4,all_canonical_equal=all(r['canonical_equal'] for g in ['runs','control_runs'] for r in v[g].values()),
    validate_memo=v['validate']['memo'],files=len(manifest),bytes=sum(x['bytes'] for x in manifest.values())),indent=2))
