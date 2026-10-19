from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import subprocess, time, json
BASE=Path(__file__).resolve().parent
ROOT=BASE.parent
def build(mode):
    path=BASE/mode
    commands=[['cmake','--build',str(path),'--config','Release','--target','openttd','--parallel','12','--','/nodeReuse:false','/p:MultiProcCL=true','/p:CL_MPCount=12'],
        [r'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/mt.exe','-manifest',str(ROOT/'os/windows/openttd.manifest'),f'-outputresource:{path}/Release/openttd.exe;#1']]
    (path/'build-commands.json').write_text(json.dumps(commands,indent=2))
    started=time.monotonic()
    with (BASE/f'build-{mode}.log').open('wb') as log:
        for command in commands: subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
    print('BUILT',mode,'seconds',time.monotonic()-started,flush=True)
with ThreadPoolExecutor(max_workers=2) as pool: list(pool.map(build,('thin','full')))
