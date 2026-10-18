from pathlib import Path
import json
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OUT=ROOT/'docs/measurements/passenger-wpr-local-policy-9ab0228d-2026-10-18'
TOOLKIT=WORK/'tools/wpt/Windows Kits/10/Windows Performance Toolkit'
until=time.monotonic()+7200
while True:
    try:
        runs=json.loads((WORK/'runs-complete.json').read_text())
        done=set(runs)=={'legacy','hyperpath'} and (WORK/'wpr-recorder-complete.txt').is_file() and (WORK/'cleanup-complete.txt').is_file()
    except (FileNotFoundError,json.JSONDecodeError):done=False
    if done:break
    if time.monotonic()>until:raise TimeoutError('Two accepted WPR runs were not completed')
    time.sleep(2)
with (WORK/'export-runner.log').open('wb') as out:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_export.py'),str(WORK),str(TOOLKIT),
        '--output-name','wpa','--local-symbols-only'],cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
print('Both WPA exports complete; marker windows audited',flush=True)
OUT.mkdir(parents=True,exist_ok=True)
with (WORK/'summary-runner.log').open('wb') as out:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_summarize.py'),str(WORK),str(OUT),
        '--export-name','wpa'],cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
subprocess.run([sys.executable,'-B',str(WORK/'analyze.py')],cwd=ROOT,check=True)
(WORK/'analysis-complete.txt').write_text('WPA tables exported and counts/timings compared\n')
