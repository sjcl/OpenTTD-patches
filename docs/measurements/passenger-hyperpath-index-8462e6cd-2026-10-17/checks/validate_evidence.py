from pathlib import Path
import csv,gzip,hashlib,json

root=Path('C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches')
evidence=root/'docs/measurements/passenger-hyperpath-index-8462e6cd-2026-10-17'
baseline=root/'docs/measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17'

def read(path):
    return json.loads(path.read_text(encoding='utf-8'))

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

summary=read(evidence/'summary.json')
identity=read(evidence/'build-identity.json')
result=dict(modes={},raw_csv_hashes={},copied_tu_hashes={},reference_hashes={},canonical_rows=0)
for mode in ['legacy','hyperpath']:
    directory=evidence/mode
    meta=read(directory/'process.json')
    before=read(baseline/mode/'process.json')
    state=read(directory/'save-integrity.json')
    assert state==read(baseline/mode/'save-integrity.json')
    assert len(state)==24 and state['payments_valid'] is True
    assert meta['exit_code']==0 and meta['simulation_timing_valid'] and meta['warm_complete']
    assert meta['hyperpath_filter_mode']=='index' and meta['hyperpath_filter'] is None
    assert meta['contamination']==[] and meta['phase_contamination']==[]
    assert meta['binary_sha256']==identity['files']['Release/openttd.exe']['sha256']
    assert meta['main_thread']['verified_mask']==1 and meta['main_thread_affinity_mask']==1
    for key in ['input_sha256','language_sha256','precomputed','hyperpath','hyperpath_filter_alternate','cold_precomputed','warm_precomputed','mid_rebuild_tick']:
        assert meta[key]==before[key],(mode,key)
    clock=read(directory/'actual-ticks.json')
    assert clock['end_tick']-clock['start_tick']==len(clock['samples'])==2048
    assert (clock['start_tick'],clock['end_tick'])==(3181119581,3181121629)
    data=summary[mode]
    assert data['lost_events']==data['lost_buffers']==0
    assert data['unresolved_product_functions']=={}
    for check in data['conservation_checks'].values():
        assert check['stack_total_ns']==check['exclusive_total_ns']
        assert check['stack_samples']==check['exclusive_samples']
    export=read(directory/'export-command.json')
    assert export['exit_code']==0
    assert export['pdb_sha256']==identity['files']['Release/openttd.pdb']['sha256']
    for name,value in export['exports'].items():
        content=gzip.decompress((directory/(name+'.gz')).read_bytes())
        assert len(content)==value['bytes'] and hashlib.sha256(content).hexdigest()==value['sha256']
        result['raw_csv_hashes'][mode+'/'+name]=value['sha256']
    result['modes'][mode]=dict(canonical_fields_equal=24,ticks=2048,contamination_events=0,lost_events=0,lost_buffers=0,unresolved_product_functions=0)
with (evidence/'canonical-comparison.csv').open(encoding='utf-8',newline='') as stream:
    rows=list(csv.DictReader(stream))
assert len(rows)==48 and all(row['equal']=='True' for row in rows)
result['canonical_rows']=len(rows)
with (evidence/'membership-before-after.csv').open(encoding='utf-8',newline='') as stream:
    membership=list(csv.DictReader(stream))
for mode in ['legacy','hyperpath']:
    rows=[row for row in membership if row['mode']==mode]
    assert len(rows)==3
    for key in ['before_inclusive_ms_per_tick','after_inclusive_ms_per_tick']:
        assert len({row[key] for row in rows})==1
result['membership_inclusive_scopes_equal']=True
for name,digest in identity['copied_translation_units'].items():
    assert hashlib.sha256(gzip.decompress((evidence/'copied-tus'/(name+'.gz')).read_bytes())).hexdigest()==digest
    result['copied_tu_hashes'][name]=digest
for name,digest in identity['reference_files'].items():
    assert sha(root/name)==digest
    result['reference_hashes'][name]=digest
(evidence/'checks/validation.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
stages=read(evidence/'stages.json')
assert stages['validate']['lookup']['validated_groups']>0 and stages['validate']['lookup']['violations']==0
for name in ['validate','alternate','legacy','hyperpath']:
    assert stages[name]['canonical_fields_equal']==24 and stages[name]['lookup']['violations']==0
with (evidence/'canonical-all-runs.csv').open(encoding='utf-8',newline='') as stream:
    rows=list(csv.DictReader(stream))
assert len(rows)==96 and all(row['equal']=='True' for row in rows)
result['all_run_canonical_rows']=len(rows)
result['validate_lookup']=stages['validate']['lookup']
result['all_run_canonical_fields_equal']={name:stages[name]['canonical_fields_equal'] for name in ['validate','alternate','legacy','hyperpath']}
for name,digest in identity['tool_scripts'].items():
    assert sha(evidence/'checks/tools'/Path(name).name)==digest,name
(evidence/'checks/validation.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
manifest={p.relative_to(evidence).as_posix():dict(bytes=p.stat().st_size,sha256=sha(p)) for p in sorted(evidence.rglob('*')) if p.is_file() and p.name!='evidence-sha256.json'}
(evidence/'evidence-sha256.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
print('Validated',len(manifest),'evidence files; raw CSV hashes, 96 canonical fields, ticks and build provenance match')
