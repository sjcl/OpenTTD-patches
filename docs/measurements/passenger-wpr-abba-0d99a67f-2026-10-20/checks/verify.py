"""Gate commit/push on all eight traces, immutable backups and staged byte hashes."""
from pathlib import Path
import gzip
import hashlib
import json
import re
import subprocess

ROOT=Path('C:/Users/bebe/.codex/worktrees/3abe/OpenTTD-patches')
WORK=Path('C:/Users/bebe/.codex/worktrees/3abe/OpenTTD-patches/build-wpr-abba-0d99a67f-2026-10-20')
NAME='passenger-wpr-abba-0d99a67f-2026-10-20'
PREFIX='docs/measurements/'+NAME+'/'
OUT=ROOT/PREFIX
REPORT='docs/'+NAME+'.md'
BASE=Path('C:/Users/bebe/.codex/worktrees/3abe/OpenTTD-patches/docs/measurements/passenger-wpr-abba-6c11c67f-2026-10-19')
def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def git(*args,input=None):return subprocess.check_output(['git',*args],cwd=ROOT,input=input)
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
manifest=load(OUT/'evidence-sha256.json')
actual={p.relative_to(OUT).as_posix() for p in OUT.rglob('*') if p.is_file()}
assert actual==set(manifest)|{'evidence-sha256.json'}
for name,item in manifest.items():
    p=OUT/name;assert p.stat().st_size==item['bytes'] and sha(p)==item['sha256'],name
assert sha(ROOT/REPORT)==load(OUT/'report-sha256.json')['sha256']
for folder,identity_name in [(OUT,'build-identity.json'),(BASE,'build-identity.json')]:
    build=load(folder/identity_name)
    assert build['asserts'] is False and build['pgo'] is False and build['lto']=='thin' and build['debug_information']=='/Z7'
    assert all('23.1.3' in v for v in build['compiler_versions'].values())
    flags=load(folder/'actual-compile-flags.json')
    assert all(flags[t]['all_required_flags_present'] for t in ['openttd_lib','openttd','openttd_test'])
    objects=load(folder/'lto-objects.json');assert objects['openttd_lib']['total_objects']==objects['openttd_lib']['bitcode_objects']==608
    assert load(folder/'pdb-preflight.json')['all_required_names_present']
    assert all(item['verified'] for item in load(folder/'manifest-verification.json').values())
    assert build['src_tree']==git('rev-parse',build['product_commit']+':src').decode().strip()
for target in ['openttd_lib','openttd','openttd_test']:
    hashes=list(load(OUT/'actual-compile-flags.json')[target]['logs'].values());assert len(hashes)==1
    assert hashlib.sha256(gzip.decompress((OUT/'checks'/(target+'-clang-command.tlog.gz')).read_bytes())).hexdigest()==hashes[0]
for name in [REPORT,'docs/passenger-wpr-runbook.md','AGENTS.md']:
    p=ROOT/name
    for link in re.findall(r'\[[^\]]+\]\(([^)]+)\)',p.read_text(encoding='utf-8')):
        if '://' not in link and not link.startswith('#'):assert (p.parent/link.split('#')[0]).is_file(),(name,link)
backup=load(OUT/'recording-backup.json')
for record in [backup]:
    for name,item in record['files'].items():
        p=Path(record['path'])/name;assert p.stat().st_size==item['bytes'] and sha(p)==item['sha256'],name
validation=load(OUT/'validation.json');assert len(validation['runs'])==validation['accepted_traces']==8
assert validation['separate_validate']['passed']
input_backup=load(OUT/'input-backup.json');assert sha(Path(input_backup['path']))==input_backup['sha256']
for key,r in validation['runs'].items():
    assert all(item['equal'] for item in r['canonical'].values()) and r['payments_valid']
    assert r['lost_events']==r['lost_buffers']==0 and r['raw_csv_hashes_match'] and r['window_valid']
    folder=OUT/key;export=load(folder/'export-command.json')
    for fn,item in export['exports'].items():
        with gzip.open(folder/(fn+'.gz'),'rb') as f:assert hashlib.file_digest(f,'sha256').hexdigest()==item['sha256']
comparison=load(OUT/'comparison.json');assert comparison['matched_clang_thinlto_conditions'] and not comparison['untouched_correction_used']
paths=git('diff','--cached','--name-only','-z').decode().strip('\0').split('\0')
expected={PREFIX+p for p in actual}|{REPORT}
assert set(paths)==expected,(set(paths)-expected,expected-set(paths))
assert not git('diff','--cached','--name-only','--diff-filter=D')
index={}
for entry in git('ls-files','--stage','-z','--',PREFIX,REPORT,'docs/.gitattributes','docs/passenger-wpr-runbook.md').decode().strip('\0').split('\0'):
    header,path=entry.split('\t',1);mode,oid,stage=header.split();assert stage=='0';index[path]=oid
hashes=git('hash-object','--no-filters','--stdin-paths',input=('\n'.join(paths)+'\n').encode()).decode().splitlines()
assert len(hashes)==len(paths) and all(index[p]==h for p,h in zip(paths,hashes)),'Staged bytes changed through Git filters'
subprocess.run(['git','diff','--cached','--check'],cwd=ROOT,check=True)
(WORK/'staged-validation.json').write_text(json.dumps(dict(files=len(paths),all_staged_bytes_match=True,manifest_valid=True,backups_verified=True),indent=2)+'\n',encoding='utf-8',newline='\n')
print('PASS: '+str(len(paths))+' staged files; all report/evidence bytes, backups, canonical and quality verified.',flush=True)
