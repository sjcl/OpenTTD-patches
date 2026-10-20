"""Archive the rejected current Hyperpath attempt without changing its evidence."""
from pathlib import Path
import json

WORK=Path(__file__).resolve().parent
source=(WORK/'hyperpath').resolve();dest=(WORK/'rejected-missing-begin-marker').resolve()
assert source.is_relative_to(WORK) and dest.parent==WORK and not dest.exists()
assert not source.is_symlink() and not source.is_junction()
markers=(source/'window-markers.txt').read_text(encoding='utf-8-sig')
assert 'hyperpath-end' in markers and 'hyperpath-begin' not in markers
source.rename(dest)
log=WORK/'export-runner.log';archived=WORK/'export-rejected-marker-runner.log'
assert not archived.exists();log.rename(archived)
(WORK/'rejected-marker.json').write_text(json.dumps(dict(reason='Missing hyperpath-begin in the ETL marks table',
    performance_accepted=False,canonical_equal=True,archived_directory=str(dest),
    action='Retake Hyperpath only from the same input and binary; preserve valid Legacy capture'),indent=2)+'\n')
print('Rejected attempt archived; fresh hyperpath output is ready',flush=True)
