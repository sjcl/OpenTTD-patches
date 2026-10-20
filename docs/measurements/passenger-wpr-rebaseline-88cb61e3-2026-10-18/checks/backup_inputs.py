"""Keep a second copy of the fixed inputs outside the disposable Git worktree."""
from pathlib import Path
import hashlib
import json
import zipfile

WORK=Path(__file__).resolve().parent
DEST=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329/passenger-wpr-inputs-88cb61e3-2026-10-18.zip')
assert json.loads((WORK/'seeds-complete.json').read_text())['complete']
assert not DEST.exists()
DEST.parent.mkdir(parents=True,exist_ok=True)
entries={}
for package in ['original','seeds/legacy','seeds/hyperpath']:
    for name in ['input.sav','openttd.cfg','package.json']:
        entries[package+'/'+name]=WORK/package/name
for name in ['seed-validation.json','seeds-complete.json']:
    entries[name]=WORK/name
for name in ['baseset','newgrf','content_download','lang']:
    for p in (WORK/'original'/name).rglob('*'):
        if p.is_file(): entries['assets/'+p.relative_to(WORK/'original').as_posix()]=p
manifest={name:dict(bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for name,p in entries.items()}
with zipfile.ZipFile(DEST,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as out:
    for name,p in entries.items(): out.write(p,name)
    out.writestr('archive-manifest.json',json.dumps(manifest,indent=2)+'\n')
    out.writestr('RESTORE.txt','Restore assets/ contents into each package directory (original, seeds/legacy, seeds/hyperpath). Each already contains its own input.sav, openttd.cfg and package.json. Validate all hashes before use.\n')
with zipfile.ZipFile(DEST) as archive:
    assert archive.testzip() is None
    for name,entry in manifest.items(): assert hashlib.sha256(archive.read(name)).hexdigest()==entry['sha256'],name
with DEST.open('rb') as f: digest=hashlib.file_digest(f,'sha256').hexdigest()
result=dict(path=str(DEST),bytes=DEST.stat().st_size,sha256=digest,verified_entries=len(entries),shared_assets_stored_once=True)
(WORK/'input-backup.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result),flush=True)
