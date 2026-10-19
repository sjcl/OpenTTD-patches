from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import subprocess, json, time
BASE=Path(__file__).resolve().parent
ROOT=BASE.parent
def run(compiler):
    build=BASE/compiler
    commands=[['cmake','-S',str(ROOT),'-B',str(build),'-DCMAKE_EXE_LINKER_FLAGS='+('/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO' + (' /DEFAULTLIB:"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/lib/clang/19/lib/windows/clang_rt.builtins-x86_64.lib"' if compiler=='clang' else ''))],
        ['cmake','--build',str(build),'--config','Release','--target','openttd','--parallel','12','--','/nodeReuse:false','/p:MultiProcCL=true','/p:CL_MPCount=12']]
    if compiler=='clang':
        commands.append([r'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/mt.exe','-manifest',str(ROOT/'os/windows/openttd.manifest'),f'-outputresource:{build}/Release/openttd.exe;#1'])
    else:
        tool=next((BASE/'toolchain-new/extracted').rglob('cl.exe')).parent
        commands[1].insert(3, '--clean-first')
        commands[1] += ['/p:CLToolPath='+str(tool), '/p:LinkToolPath='+str(tool), '/p:LibToolPath='+str(tool)]
    started=time.monotonic()
    (build/'build-commands.json').write_text(json.dumps(commands,indent=2))
    with (BASE/f'build-{compiler}-public-pdb.log').open('wb') as log:
        for cmd in commands: subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
    print('BUILT',compiler,'seconds',time.monotonic()-started,flush=True)
with ThreadPoolExecutor(max_workers=2) as pool: list(pool.map(run,('msvc','clang')))
