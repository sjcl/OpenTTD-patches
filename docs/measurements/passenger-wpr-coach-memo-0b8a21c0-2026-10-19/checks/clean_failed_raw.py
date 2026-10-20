"""Keep all thin rejected evidence; remove only failed captures' large raw files."""
from pathlib import Path
import json
import os
import stat
WORK=Path(__file__).resolve().parent
assert not any(json.loads(p.read_text(encoding='utf-8-sig')).get('kind')=='map' and
    json.loads(p.read_text(encoding='utf-8-sig')).get('valid') for p in (WORK/'sampling-jobs').glob('*/quality.json')), 'Accepted final traces exist; refuse historical cleanup'
folders=['rejected-incomplete-save-replay','rejected-legacy-memory-markers','rejected-hyperpath-dropped-events','legacy','hyperpath']
proof=[]
for name in folders:
    folder=(WORK/name).resolve();assert folder.parent==WORK and not folder.is_symlink() and not folder.is_junction()
    # All five attempts are already known to be rejected; there is no active game/recorder.
    for file in folder.iterdir():
        if file.is_file() and file.suffix.lower() in ['.etl','.sav','.exe']:
            target=file.resolve();assert target.parent==folder
            proof.append(dict(path=str(target),bytes=target.stat().st_size,reason='Rejected capture; metadata and logs retained'))
            os.chmod(target,stat.S_IWRITE|stat.S_IREAD);target.unlink()
(WORK/'failed-raw-cleanup.json').write_text(json.dumps(proof,indent=2)+'\n')
print(json.dumps(dict(removed_files=len(proof),logical_bytes=sum(x['bytes'] for x in proof)),indent=2))
