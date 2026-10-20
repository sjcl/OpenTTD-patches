"""Export/summarize the eight accepted ABBA captures, reusing successful postprocessing."""
from pathlib import Path
import subprocess
import hashlib
import json
import sys
ROOT=Path('C:/Users/bebe/.codex/worktrees/3abe/OpenTTD-patches')
WORK=Path('C:/Users/bebe/.codex/worktrees/3abe/OpenTTD-patches/build-wpr-abba-0d99a67f-2026-10-20')
OUT=ROOT/'docs/measurements/passenger-wpr-abba-0d99a67f-2026-10-20'
TOOLKIT=Path('C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-wpr-9ab0228d/tools/wpt/Windows Kits/10/Windows Performance Toolkit')
assert (WORK/'captures-complete.txt').is_file()
OUT.mkdir(exist_ok=True)
with (WORK/'export-runner.log').open('ab') as log:
    for name in ['01-A1','02-B1','03-B2','04-A2']:
        folder=WORK/name;dest=OUT/name;dest.mkdir(exist_ok=True)
        for mode in ['legacy','hyperpath']:
            if not (folder/mode/'wpa').is_dir():
                subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_export.py'),str(folder),str(TOOLKIT),'--mode',mode,
                    '--output-name','wpa','--local-symbols-only'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
            identity=json.loads((folder/mode/'export-command.json').read_text(encoding='utf-8'))
            assert identity['exit_code']==0 and identity['exports'],'Existing export incomplete; fix only postprocessing'
            for fn,item in identity['exports'].items():
                with (folder/mode/'wpa'/fn).open('rb') as f:assert hashlib.file_digest(f,'sha256').hexdigest()==item['sha256']
        subprocess.run([sys.executable,'-B',str(ROOT/'utils/passenger_wpr_summarize.py'),str(folder),str(dest),'--export-name','wpa'],
            cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
        print(name+': both modes exported and summarized',flush=True)
(WORK/'post-complete.txt').write_text('Eight exports/summaries complete\n',encoding='utf-8',newline='\n')
