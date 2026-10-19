"""Postprocess accepted captures without ever launching another simulation."""
from pathlib import Path
import subprocess
import sys
import time
WORK=Path(__file__).resolve().parent
until=time.monotonic()+7200
while not (WORK/'post-complete.txt').is_file():
    if time.monotonic()>until:raise TimeoutError('WPA postprocessing did not finish')
    time.sleep(1)
for name in ['analyze.py','package.py']:
    with (WORK/(name.replace('.py','')+'-runner.log')).open('wb') as output:
        subprocess.run([sys.executable,'-B',str(WORK/name)],stdout=output,stderr=subprocess.STDOUT,check=True)
print('Analysis, report and recording backups completed',flush=True)
