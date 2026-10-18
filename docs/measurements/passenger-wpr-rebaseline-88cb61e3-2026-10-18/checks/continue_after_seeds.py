from pathlib import Path
import subprocess
import sys
import time

WORK=Path(__file__).resolve().parent
until=time.monotonic()+1800
while not (WORK/'seeds-complete.json').is_file():
    if time.monotonic()>until: raise TimeoutError('Seed reload verification did not finish')
    time.sleep(2)
for name in ['backup_inputs.py','measure_matrix.py']:
    print('Starting '+name,flush=True)
    subprocess.run([sys.executable,'-B',str(WORK/name)],cwd=WORK.parent,check=True)
