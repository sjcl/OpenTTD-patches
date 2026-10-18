from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=Path(__file__).resolve().parent
TOOLKIT=WORK/'tools/wpt/Windows Kits/10/Windows Performance Toolkit'
assert (WORK/'matrix-recorder-complete.txt').is_file()
for version in ['baseline','candidate']:
    build=WORK/version
    if version=='candidate':
        for name in ['symcache','symbols']:
            shutil.copytree(WORK/'baseline'/name,build/name,dirs_exist_ok=True)
    with (build/'export-runner.log').open('wb') as out:
        subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_export.py'),str(build),str(TOOLKIT),
            '--output-name','wpa',*(['--local-symbols-only'] if version=='candidate' else [])],cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
    print(version+' exports completed and marker windows audited',flush=True)
    evidence=ROOT/'docs/measurements/passenger-wpr-rebaseline-88cb61e3-2026-10-18'/version
    evidence.mkdir(parents=True,exist_ok=True)
    with (build/'summary-runner.log').open('wb') as out:
        subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_summarize.py'),str(build),str(evidence),
            '--export-name','wpa'],cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
    print(version+' inclusive/exclusive summaries completed',flush=True)
