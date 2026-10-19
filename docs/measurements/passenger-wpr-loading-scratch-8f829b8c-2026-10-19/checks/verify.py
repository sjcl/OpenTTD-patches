"""Gate commit/push on report, manifest and staged byte identity."""
from pathlib import Path
import hashlib
import json
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
NAME='passenger-wpr-loading-scratch-8f829b8c-2026-10-19'
PREFIX='docs/measurements/'+NAME+'/'
OUT=ROOT/PREFIX
REPORT='docs/'+NAME+'.md'
def load(path):return json.loads(path.read_text(encoding='utf-8-sig'))
def git(*args,input=None):return subprocess.check_output(['git',*args],cwd=ROOT,input=input)
def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

manifest=load(OUT/'evidence-sha256.json')
actual={p.relative_to(OUT).as_posix() for p in OUT.rglob('*') if p.is_file()}
assert actual==set(manifest)|{'evidence-sha256.json'}
for name,item in manifest.items():
    p=OUT/name;assert p.stat().st_size==item['bytes'] and sha(p)==item['sha256'],name
assert sha(ROOT/REPORT)==load(OUT/'report-sha256.json')['sha256']
for name in [REPORT,'docs/passenger-wpr-runbook.md','AGENTS.md']:
    p=ROOT/name
    for link in re.findall(r'\[[^\]]+\]\(([^)]+)\)',p.read_text(encoding='utf-8')):
        if '://' not in link and not link.startswith('#'):assert (p.parent/link.split('#')[0]).is_file(),(name,link)
backup=load(OUT/'recording-backup.json')
for name,item in backup['files'].items():
    p=Path(backup['path'])/name;assert p.stat().st_size==item['bytes'] and sha(p)==item['sha256'],name
assert load(OUT/'build-identity.json')['src_tree']==git('rev-parse','8f829b8c:src').decode().strip()
for mode,result in load(OUT/'validation.json')['runs'].items():
    assert all(item['equal'] for item in result['canonical'].values()) and result['payments_valid']
    assert result['lost_events']==result['lost_buffers']==0 and result['raw_csv_hashes_match']
paths=git('diff','--cached','--name-only','-z').decode().strip('\0').split('\0')
expected={PREFIX+p for p in actual}|{REPORT,'docs/.gitattributes','docs/passenger-wpr-runbook.md'}
assert set(paths)==expected,(len(paths),len(expected))
assert not git('diff','--cached','--name-only','--diff-filter=D')
index={}
for entry in git('ls-files','--stage','-z','--',PREFIX,REPORT,'docs/.gitattributes','docs/passenger-wpr-runbook.md').decode().strip('\0').split('\0'):
    header,path=entry.split('\t',1);mode,oid,stage=header.split();assert stage=='0';index[path]=oid
hashes=git('hash-object','--no-filters','--stdin-paths',input=('\n'.join(paths)+'\n').encode()).decode().splitlines()
assert len(hashes)==len(paths) and all(index[p]==h for p,h in zip(paths,hashes)),'Git changed staged bytes'
subprocess.run(['git','diff','--cached','--check'],cwd=ROOT,check=True)
(WORK/'staged-validation.json').write_text(json.dumps(dict(files=len(paths),all_staged_bytes_match=True,manifest_valid=True,backups_verified=True),indent=2)+'\n',encoding='utf-8',newline='\n')
print('PASS: '+str(len(paths))+' staged files; all report/evidence hashes, links, backups and quality checks verified.',flush=True)
