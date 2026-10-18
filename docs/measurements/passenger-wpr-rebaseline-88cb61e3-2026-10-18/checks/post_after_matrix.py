from pathlib import Path
import json
import subprocess
import sys
import time

WORK=Path(__file__).resolve().parent
until=time.monotonic()+7200
while True:
    try:
        canonical=json.loads((WORK/'canonical-comparison.json').read_text())
        done=(WORK/'matrix-recorder-complete.txt').is_file() and set(canonical)=={'legacy','hyperpath'} and all(x['equal'] for x in canonical.values())
    except (FileNotFoundError,json.JSONDecodeError): done=False
    if done:break
    if time.monotonic()>until:raise TimeoutError('The complete matched WPR matrix was not produced')
    time.sleep(2)
for name in ['package_setup.py','export_matrix.py','analyze_matrix.py','validate_matrix.py']:
    print('Starting '+name,flush=True)
    subprocess.run([sys.executable,'-B',str(WORK/name)],cwd=WORK.parent,check=True)
(WORK/'postprocessing-complete.txt').write_text('WPR exports, canonical equality and conservation verified\n')
