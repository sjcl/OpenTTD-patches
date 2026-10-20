"""Summarize the matched controls after creating their evidence parent, then compare."""
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
OUT=ROOT/'docs/measurements/passenger-wpr-coach-memo-0b8a21c0-2026-10-19'
(OUT/'control-97cdc0ad').mkdir(parents=True,exist_ok=True)
with (WORK/'control-summary-runner.log').open('wb') as out:
    subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_summarize.py'),str(WORK/'control-97cdc0ad'),str(OUT/'control-97cdc0ad'),
        '--export-name','wpa'],cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
subprocess.run([sys.executable,'-B',str(WORK/'analyze.py')],cwd=ROOT,check=True)
(WORK/'analysis-complete.txt').write_text('Four matched traces exported and compared\n')
