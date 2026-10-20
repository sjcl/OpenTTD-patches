"""Export only the new Hyperpath trace, then summarize both valid captures."""
from pathlib import Path
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OUT=ROOT/'docs/measurements/passenger-wpr-local-policy-9ab0228d-2026-10-18'
TOOLKIT=WORK/'tools/wpt/Windows Kits/10/Windows Performance Toolkit'
deadline=time.monotonic()+7200
while not ((WORK/'hyperpath-retry-complete.txt').is_file() and (WORK/'wpr-retry-recorder-complete.txt').is_file()):
    if time.monotonic()>deadline:raise TimeoutError('Retry did not complete')
    time.sleep(2)
with (WORK/'export-runner.log').open('wb') as out:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_export.py'),str(WORK),str(TOOLKIT),
        '--mode','hyperpath','--output-name','wpa','--local-symbols-only'],cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
OUT.mkdir(parents=True,exist_ok=True)
with (WORK/'summary-runner.log').open('wb') as out:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_summarize.py'),str(WORK),str(OUT),'--export-name','wpa'],
        cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
subprocess.run([sys.executable,'-B',str(WORK/'analyze.py')],cwd=ROOT,check=True)
(WORK/'analysis-complete.txt').write_text('Both accepted captures exported; canonical and CPU time compared\n')
