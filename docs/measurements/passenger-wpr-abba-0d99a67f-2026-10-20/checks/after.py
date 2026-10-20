from pathlib import Path
import time,subprocess,sys
ROOT=Path('C:/Users/bebe/.codex/worktrees/3abe/OpenTTD-patches');WORK=ROOT/'build-wpr-abba-0d99a67f-2026-10-20'
until=time.monotonic()+7200
while not (WORK/'captures-complete.txt').is_file() or not (WORK/'recorder-complete.txt').is_file():
 if (WORK/'recorder-stop.txt').exists() and not (WORK/'captures-complete.txt').exists():raise RuntimeError('Capture queue stopped before completion')
 if time.monotonic()>until:raise TimeoutError('Capture completion wait')
 time.sleep(1)
for script in ['validate.py','post.py','analyze.py','package.py']:
 print('Starting '+script,flush=True)
 subprocess.run([sys.executable,'-B',str(WORK/script)],cwd=ROOT,check=True)
