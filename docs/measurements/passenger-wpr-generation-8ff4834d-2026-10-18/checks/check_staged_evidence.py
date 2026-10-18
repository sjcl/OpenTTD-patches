"""Confirm every staged evidence blob preserves the inventoried bytes."""
from pathlib import Path
import hashlib
import json
import subprocess

ROOT=Path(__file__).resolve().parents[1]
NAME='passenger-wpr-generation-8ff4834d-2026-10-18'
PREFIX='docs/measurements/'+NAME+'/'
REPORT='docs/'+NAME+'.md'
def git(*args,input=None):return subprocess.check_output(['git',*args],input=input,cwd=ROOT)
paths=git('diff','--cached','--name-only','-z').decode().strip('\0').split('\0')
assert paths and all(p.startswith(PREFIX) or p==REPORT for p in paths), 'Unexpected staged files; preserve them and inspect before committing'
assert not git('diff','--cached','--name-only','--diff-filter=D')
index={}
for line in git('ls-files','--stage','-z','--',PREFIX,REPORT).decode().strip('\0').split('\0'):
    header,path=line.split('\t',1)
    mode,oid,stage=header.split()
    assert stage=='0'
    index[path]=oid
assert set(index)==set(paths)
manifest=json.loads((ROOT/PREFIX/'evidence-sha256.json').read_text())
assert set(paths)=={PREFIX+p for p in manifest}|{PREFIX+'evidence-sha256.json',REPORT}
for name,item in manifest.items():
    p=ROOT/PREFIX/name
    assert p.stat().st_size==item['bytes']
    assert hashlib.sha256(p.read_bytes()).hexdigest()==item['sha256'],name
report_info=json.loads((ROOT/PREFIX/'report-sha256.json').read_text())
assert hashlib.sha256((ROOT/REPORT).read_bytes()).hexdigest()==report_info['sha256']
actual=git('hash-object','--no-filters','--stdin-paths',input=('\n'.join(paths)+'\n').encode()).decode().splitlines()
assert len(actual)==len(paths)
assert all(index[p]==h for p,h in zip(paths,actual)), 'Git staging transformed bytes'
proof=dict(files=len(paths),staged_blobs_equal_working_files=True,parent=git('rev-parse','HEAD').decode().strip(),blobs={p:index[p] for p in sorted(paths)})
(Path(__file__).resolve().parent/'staged-validation.json').write_text(json.dumps(proof,indent=2)+'\n')
print(json.dumps({k:v for k,v in proof.items() if k!='blobs'},indent=2))
