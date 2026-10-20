"""Export exact marked windows and compare against the retained 9ab0228d evidence."""
from pathlib import Path
import json
import subprocess
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OUT=ROOT/'docs/measurements/passenger-wpr-waiting-index-97cdc0ad-2026-10-19'
TOOLKIT=ROOT/'build-wpr-9ab0228d/tools/wpt/Windows Kits/10/Windows Performance Toolkit'
deadline=time.monotonic()+7200
while True:
    if (WORK/'runs-complete.json').is_file() and (WORK/'wpr-recorder-complete.txt').is_file():
        if set(json.loads((WORK/'runs-complete.json').read_text()))=={'legacy','hyperpath'}:break
    if time.monotonic()>deadline:raise TimeoutError('Captures did not complete')
    time.sleep(2)
with (WORK/'export-runner.log').open('wb') as out:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_export.py'),str(WORK),str(TOOLKIT),
        '--output-name','wpa','--local-symbols-only'],cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
OUT.mkdir(parents=True,exist_ok=True)
with (WORK/'summary-runner.log').open('wb') as out:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_summarize.py'),str(WORK),str(OUT),'--export-name','wpa'],cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
subprocess.run([sys.executable,'-B',str(WORK/'analyze.py')],cwd=ROOT,check=True)
(WORK/'analysis-complete.txt').write_text('Both accepted captures exported and compared\n')
