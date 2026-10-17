"""Archive the validation/A-B replays and compare index versus mask walk timing."""
from pathlib import Path
import csv,gzip,hashlib,json,re,shutil

root=Path('C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches')
build=root/'build-wpr-2d51a9ca'
evidence=root/'docs/measurements/passenger-hyperpath-index-2d51a9ca-2026-10-17'
baseline=root/'docs/measurements/passenger-hyperpath-wpr-67cbbf32-2026-10-17'

def compress(source,target):
    with source.open('rb') as src,target.open('wb') as dst:
        with gzip.GzipFile(filename='',fileobj=dst,mode='wb',mtime=0) as out:
            shutil.copyfileobj(src,out)

def diagnostics(name):
    directory=build/name
    metadata=json.loads((directory/'process.json').read_text(encoding='utf-8'))
    clock=metadata['actual-ticks.json']
    assert metadata['exit_code']==0 and metadata['simulation_timing_valid'] and metadata['warm_complete']
    assert not metadata['contamination'] and not metadata['phase_contamination']
    assert clock['end_tick']-clock['start_tick']==len(clock['samples'])==2048
    assert metadata['save-integrity.json']==json.loads((baseline/('legacy' if name=='legacy' else 'hyperpath')/'save-integrity.json').read_text(encoding='utf-8'))
    text=(directory/'on-1-console.txt').read_text(encoding='utf-8',errors='replace')
    found={}
    for kind,label in [('filter','Passenger hyperpath boarding filter'),('time','Passenger hyperpath walk time'),('lookup','Passenger hyperpath lookup')]:
        lines=re.findall(re.escape(label)+r': ([^\r\n]+)',text)
        assert lines
        found[kind]={k:(int(v) if v.isdigit() else v) for k,v in re.findall(r'(\w+)=([^; ]+)',lines[-1])}
        found[kind+'_line']=label+': '+lines[-1]
    assert found['lookup']['violations']==0
    if name=='validate':
        assert found['filter']['mode']=='validate' and found['lookup']['validated_groups']>0
    elif name=='alternate':
        assert found['filter']['mode']=='alternate'
        assert found['filter']['filtered_walks']>0 and found['filter']['indexed_walks']>0
        assert found['filter']['unfiltered_walks']==0
    else:
        assert found['filter']['mode']=='index'
    found.update(ticks=2048,canonical_fields_equal=24,
                 rebuilds_per_tick=found['lookup']['rebuilds']/2048,
                 reindexed_groups_per_tick=found['lookup']['indexed_groups']/2048)
    if name in ['validate','alternate']:
        output=evidence/name
        output.mkdir(exist_ok=True)
        for file in ['process.json','actual-ticks.json','save-integrity.json']:
            shutil.copy2(directory/file,output/file)
        for file in ['on-1-console.txt','stdout.txt','stderr.txt']:
            compress(directory/file,output/(file+'.gz'))
        shutil.copytree(directory/'scripts',output/'scripts',dirs_exist_ok=True)
    return found

data={name:diagnostics(name) for name in ['validate','alternate','legacy','hyperpath']}
canonical=[]
for name in data:
    after=json.loads((build/name/'save-integrity.json').read_text(encoding='utf-8'))
    before=json.loads((baseline/('legacy' if name=='legacy' else 'hyperpath')/'save-integrity.json').read_text(encoding='utf-8'))
    for key in sorted(before.keys()|after.keys()):
        canonical.append(dict(run=name,field=key,before=before.get(key),after=after.get(key),equal=key in before and key in after and before[key]==after[key]))
assert len(canonical)==96 and all(row['equal'] for row in canonical)
with (evidence/'canonical-all-runs.csv').open('w',encoding='utf-8',newline='') as stream:
    writer=csv.DictWriter(stream,fieldnames=list(canonical[0]),lineterminator='\n')
    writer.writeheader();writer.writerows(canonical)
reference_console=gzip.decompress((baseline/'hyperpath/on-1-console.txt.gz').read_bytes()).decode('utf-8',errors='replace')
reference_line=re.findall(r'Passenger hyperpath boarding filter: ([^\r\n]+)',reference_console)[-1]
reference={k:(int(v) if v.isdigit() else v) for k,v in re.findall(r'(\w+)=([^; ]+)',reference_line)}
for name in ['validate','alternate','hyperpath']:
    counts=data[name]['filter']
    assert sum(counts[k] for k in ['filtered_walks','unfiltered_walks','indexed_walks'])==reference['filtered_walks']
    assert counts['groups_admitted']==reference['groups_admitted']
data['reference_67cbbf32_filter']=reference
a=data['alternate']
rows=[]
for mode in ['filtered','indexed']:
    walks=a['filter'][mode+'_walks']
    elapsed=a['time'][mode+'_us']
    groups=a['time'][mode+'_groups']
    rows.append(dict(mode=mode,walks=walks,total_us=elapsed,groups=groups,us_per_walk=elapsed/walks,groups_per_walk=groups/walks))
mask,index=rows
rate=(mask['walks']+index['walks'])/2048
data['alternate_comparison']=dict(rows=rows,walks_per_tick=rate,
    saved_us_per_walk=mask['us_per_walk']-index['us_per_walk'],
    estimated_saved_ms_per_tick=(mask['us_per_walk']-index['us_per_walk'])*rate/1000,
    mixed_measured_walk_ms_per_tick=(mask['total_us']+index['total_us'])/2048/1000)
with (evidence/'alternate-walks.csv').open('w',encoding='utf-8',newline='') as stream:
    writer=csv.DictWriter(stream,fieldnames=list(rows[0]),lineterminator='\n')
    writer.writeheader();writer.writerows(rows)
(evidence/'stages.json').write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
print(json.dumps(data,indent=2))
