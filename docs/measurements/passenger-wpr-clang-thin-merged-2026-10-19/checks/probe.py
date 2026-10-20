"""Check native PDB module/inline names before permitting expensive WPR runs."""
from pathlib import Path
import json
import subprocess
import time
WORK=Path(__file__).resolve().parent
PDBUTIL=Path('C:/Program Files/LLVM/bin/llvm-pdbutil.exe')
until=time.monotonic()+7200
while not (WORK/'build-ready.txt').is_file():
    if time.monotonic()>until:raise TimeoutError('Build did not finish')
    time.sleep(1)
pdb=WORK/'Release/openttd.pdb'
needles=['StateGameLoop','CanBoard','MayUseHyperpathPolicy','PlanPrecomputed','VisitBoardingCandidates']
matches={key:[] for key in needles};commands=[]
for kind in ['--symbols','--ids']:
    if all(matches.values()):break
    command=[str(PDBUTIL),'dump',kind,str(pdb)];commands.append(command)
    process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',errors='replace')
    for line in process.stdout:
        for key in needles:
            if key in line and len(matches[key])<3:matches[key].append(line.rstrip())
    errors=process.stderr.read();assert process.wait()==0,errors
text='\n'.join(key+':\n'+'\n'.join(lines) for key,lines in matches.items())+'\n'
(WORK/'pdb-scope-check.txt').write_text(text,encoding='utf-8',newline='\n')
(WORK/'pdb-preflight.json').write_text(json.dumps(dict(commands=commands,matches=matches,all_required_names_present=all(matches.values()),
    method='Native llvm-pdbutil module symbols and, when needed, inlinee IDs; no DIA registry dependency or forced noinline.'),indent=2)+'\n',encoding='utf-8',newline='\n')
assert all(matches.values()),'Required game/passenger PDB names missing; do not start captures'
(WORK/'symbols-ready.txt').write_text('Internal game/passenger names found in the matching PDB\n',encoding='utf-8',newline='\n')
print('PDB preflight passed: '+', '.join(needles),flush=True)
