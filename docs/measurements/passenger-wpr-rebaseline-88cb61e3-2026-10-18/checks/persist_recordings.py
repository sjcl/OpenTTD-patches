"""Retain immutable ETLs and matching PDBs outside the disposable worktree."""
from pathlib import Path
import hashlib
import json
import os
import shutil

WORK=Path(__file__).resolve().parent
ROOT=WORK.parent
PUBLIC=ROOT/'docs/measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18'
DEST=Path('C:/Users/bebe/.codex/visualizations/2026/10/17/01a14897-d705-7e72-a972-7f5e9758a329/passenger-wpr-recordings-88cb61e3-2026-10-18')
assert (PUBLIC/'validation.json').is_file()
assert not DEST.exists()
DEST.mkdir(parents=True)

def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
entries={}
for version in ['baseline','candidate']:
    for mode in ['legacy','hyperpath']:
        source=WORK/version/mode/'cpu.etl';dest=DEST/version/mode/'cpu.etl';dest.parent.mkdir(parents=True)
        try:os.link(source,dest);method='NTFS hard link; survives removal of the worktree link'
        except OSError:shutil.copy2(source,dest);method='copy'
        expected=json.loads((WORK/version/mode/'export-command.json').read_text())['trace_sha256']
        assert sha(dest)==expected
        entries[dest.relative_to(DEST).as_posix()]=dict(bytes=dest.stat().st_size,sha256=expected,method=method)
        for name in ['process.json','actual-ticks.json','save-integrity.json','window-audit.json']:
            p=WORK/version/mode/name;q=dest.parent/name;shutil.copy2(p,q)
    for name in ['Release/openttd.exe','Release/openttd.pdb','cpu-sampled.wpaProfile']:
        p=WORK/version/name;q=DEST/version/name;q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q)
        assert sha(q)==sha(p)
        entries[q.relative_to(DEST).as_posix()]=dict(bytes=q.stat().st_size,sha256=sha(q),method='copy')
shutil.copy2(WORK/'source-8462e6cd.tar',DEST/'source-8462e6cd.tar')
shutil.copy2(WORK/'input-backup.json',DEST/'input-backup.json')
(DEST/'recordings-manifest.json').write_text(json.dumps(entries,indent=2)+'\n')
result=dict(path=str(DEST),immutable_trace_links=True,matching_binary_and_pdb_copied=True,files=entries)
(WORK/'recording-backup.json').write_text(json.dumps(result,indent=2)+'\n')
shutil.copy2(WORK/'recording-backup.json',PUBLIC/'recording-backup.json')
print('Raw ETLs and matching PDBs retained outside the worktree:',DEST,flush=True)
