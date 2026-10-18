"""Append validated replacement-build traces to the durable normal-run archive."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import stat

SCRIPTS=Path(__file__).resolve().parent
WORK=SCRIPTS/'rebuild'
PUBLIC=SCRIPTS.parent/'docs/measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18/rebuild'
prior=json.loads((SCRIPTS/'recording-backup.json').read_text())
DEST=Path(prior['path'])/'rebuild'
validation=json.loads((PUBLIC/'validation.json').read_text())
assert len(validation['runs'])==4 and all(r['kernel_window_valid'] for r in validation['runs'].values())
assert not DEST.exists()
DEST.mkdir(parents=True)

def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

entries={}
for version in ['baseline','candidate']:
    # The supplemental run uses exactly the retained normal-run binary/PDB.
    for name in ['Release/openttd.exe','Release/openttd.pdb','cpu-sampled.wpaProfile']:
        assert sha(WORK/version/name)==sha(DEST.parent/version/name)
    for mode in ['legacy','hyperpath']:
        source=WORK/version/mode/'cpu.etl'
        dest=DEST/version/mode/'cpu.etl'
        dest.parent.mkdir(parents=True)
        try:
            os.link(source,dest)
            method='NTFS hard link; survives removal of the worktree link'
        except OSError:
            shutil.copy2(source,dest)
            method='copy'
        expected=json.loads((source.parent/'export-command.json').read_text())['trace_sha256']
        assert sha(dest)==expected
        dest.chmod(stat.S_IREAD)
        entries[dest.relative_to(DEST).as_posix()]=dict(bytes=dest.stat().st_size,sha256=expected,method=method,read_only=True)
        for name in ['process.json','actual-ticks.json','save-integrity.json','window-audit.json']:
            p=source.parent/name
            q=dest.parent/name
            shutil.copy2(p,q)
            assert sha(q)==sha(p)
            entries[q.relative_to(DEST).as_posix()]=dict(bytes=q.stat().st_size,sha256=sha(q),method='copy')
result=dict(path=str(DEST),scenario='steady seed; transfer penalty 20 -> 17; ordinary tick replacement',
    shared_binary_and_pdb_archive=str(DEST.parent),immutable_trace_links=True,files=entries)
(DEST/'recordings-manifest.json').write_text(json.dumps(result,indent=2)+'\n')
(WORK/'recording-backup.json').write_text(json.dumps(result,indent=2)+'\n')
shutil.copy2(WORK/'recording-backup.json',PUBLIC/'recording-backup.json')
for name,entry in prior['files'].items():
    if name.endswith('.etl'):
        (DEST.parent/name).chmod(stat.S_IREAD)
        entry['read_only']=True
prior['traces_read_only']=True
(DEST.parent/'recordings-manifest.json').write_text(json.dumps(prior['files'],indent=2)+'\n')
(SCRIPTS/'recording-backup.json').write_text(json.dumps(prior,indent=2)+'\n')
shutil.copy2(SCRIPTS/'recording-backup.json',PUBLIC.parent/'recording-backup.json')
print('Supplemental ETLs verified and retained outside the worktree:',DEST)
