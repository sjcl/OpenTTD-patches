"""Preserve the aborted cold attempt; rerun only after its owned recorder has stopped."""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import time
WORK=Path(__file__).resolve().parent
source=WORK/'legacy';destination=WORK/'rejected-incomplete-save-replay'
assert source.resolve().parent==destination.resolve().parent==WORK and not destination.exists()
assert not source.is_symlink() and not source.is_junction()
assert json.loads((source/'aborted.json').read_text(encoding='utf-8-sig'))['performance_accepted'] is False
deadline=time.monotonic()+1200
while not (source/'wpr-saved.txt').is_file():
    if time.monotonic()>deadline:raise TimeoutError('Rejected WPR did not finish saving')
    time.sleep(2)
source.rename(destination)
for name in ['measure-runner.log','legacy-runner.log']:
    shutil.copy2(WORK/name,WORK/('rejected-'+name))
with (WORK/'measure-runner.log').open('wb') as out:
    subprocess.run([sys.executable,'-B',str(WORK/'measure.py')],stdout=out,stderr=subprocess.STDOUT,check=True)
