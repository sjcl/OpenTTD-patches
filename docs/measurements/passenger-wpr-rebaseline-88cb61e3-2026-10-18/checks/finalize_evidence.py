"""Validate the eight accepted captures and inventory the final publishable evidence."""
from pathlib import Path
import hashlib
import json
import re

ROOT=Path(__file__).resolve().parents[1]
NAME='passenger-wpr-rebaseline-88cb61e3-2026-10-18'
OUT=ROOT/'docs/measurements'/NAME
REPORT=ROOT/'docs'/(NAME+'.md')
def load(p):return json.loads(p.read_text(encoding='utf-8'))
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
identities=load(OUT/'build-identities.json')
seeds=load(OUT/'seed-validation.json')
proof=dict(accepted_traces=8,matched_canonical_comparisons=4,canonical_fields=24,requested_ticks=2048,runs={})
for scenario in ['', 'rebuild']:
    base=OUT/scenario
    validation=load(base/'validation.json')
    assert len(validation['runs'])==4
    canonical=load(base/'canonical-comparison.json')
    assert set(canonical)=={'legacy','hyperpath'} and all(x['equal'] and x['fields']==24 for x in canonical.values())
    for mode in ['legacy','hyperpath']:
        a=load(base/'baseline'/mode/'save-integrity.json')
        b=load(base/'candidate'/mode/'save-integrity.json')
        assert a==b and len(a)==24
        for version in ['baseline','candidate']:
            p=base/version/mode
            meta=load(p/'process.json')
            assert meta['binary_sha256']==identities[version]['files']['Release/openttd.exe']['sha256']
            assert meta['input_sha256']==seeds[mode]['input_sha256']
            assert meta['rebuild_precomputed']==bool(scenario)
            assert meta['exit_code']==0 and meta['simulation_timing_valid'] is True
            ticks=meta['actual-ticks.json']
            assert ticks['start_tick']==13647872 and ticks['end_tick']==13649920 and len(ticks['samples'])==2048
            assert load(p/'window-audit.json')['valid'] is True
            if scenario:
                assert meta['save-integrity.json']['table_operations']>0
            else:
                assert meta['save-integrity.json']['table_operations']==seeds[mode]['canonical']['table_operations']
            proof['runs']['/'.join([scenario or 'normal',version,mode])]=dict(binary_and_input_verified=True,exact_ticks=2048)
    assert load(base/'recording-backup.json')['path']
for scenario,label in [('', 'Normal steady runs: zero table construction Work'),('rebuild','Replacement build: transfer penalty 20 -> 17')]:
    (OUT/scenario/'README.md').write_text(f'# {label}\n\nSee the [complete report]({"../../../" if scenario else "../../"}{NAME}.md).\n\n`function-before-after.csv` contains raw and corrected inclusive/exclusive CPU ms/tick.\n`baseline/` and `candidate/` contain normalized function CSVs, WPA raw CSV gzip files and metadata.\n`validation.json` verifies the exact 2,048 ticks, kernel process windows, lost events and export conservation.\n',encoding='utf-8')
for link in re.findall(r'\]\(([^)]+)\)',REPORT.read_text(encoding='utf-8')):
    if '://' not in link:
        assert (REPORT.parent/link.split('#')[0]).exists(), link
(OUT/'checks/final-validation.json').write_text(json.dumps(proof,indent=2)+'\n')
report_bytes=REPORT.read_bytes().replace(b'\r\n',b'\n')
(OUT/'report-sha256.json').write_text(json.dumps(dict(path='../../'+REPORT.name,
    representation='Git blob: UTF-8 with LF; checkout may convert to CRLF',
    bytes=len(report_bytes),sha256=hashlib.sha256(report_bytes).hexdigest()),indent=2)+'\n')
manifest={}
for p in sorted(OUT.rglob('*')):
    if not p.is_file() or p.name=='evidence-sha256.json':continue
    assert p.suffix.lower() not in ['.sav','.exe','.pdb','.etl'], p
    assert p.stat().st_size<100_000_000, p
    manifest[p.relative_to(OUT).as_posix()]=dict(bytes=p.stat().st_size,sha256=sha(p))
(OUT/'evidence-sha256.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(dict(accepted_traces=8,canonical_comparisons=4,canonical_fields=24,files=len(manifest),bytes=sum(x['bytes'] for x in manifest.values())),indent=2))
