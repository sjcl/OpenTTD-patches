"""Export only after both captures passed, then summarize and compare to 0b8a21c0."""
from pathlib import Path
import json
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OUT=ROOT/'docs/measurements/passenger-wpr-clang-thin-merged-2026-10-19'
BASE=ROOT/'docs/measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19'
TOOLKIT=ROOT/'build-wpr-9ab0228d/tools/wpt/Windows Kits/10/Windows Performance Toolkit'
until=time.monotonic()+7200
while not (WORK/'captures-complete.txt').is_file():
    if time.monotonic()>until or ((WORK/'recorder-stop.txt').is_file() and not (WORK/'captures-complete.txt').is_file()):raise RuntimeError('Captures did not complete successfully')
    time.sleep(1)
with (WORK/'export-runner.log').open('wb') as output:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_export.py'),str(WORK),str(TOOLKIT),
                    '--output-name','wpa','--local-symbols-only'],cwd=ROOT,stdout=output,stderr=subprocess.STDOUT,check=True)
OUT.mkdir(parents=True,exist_ok=False)
with (WORK/'summary-runner.log').open('wb') as output:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_summarize.py'),str(WORK),str(OUT),'--export-name','wpa'],
                   cwd=ROOT,stdout=output,stderr=subprocess.STDOUT,check=True)
(WORK/'post-complete.txt').write_text('Both traces exported and summarized\n',encoding='utf-8',newline='\n')
print('Both LLVM ThinLTO traces exported and summarized; no untouched correction across compiler changes.',flush=True)
