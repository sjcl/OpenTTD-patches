from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import json,subprocess,shutil,sys
ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'build-clang-thin-pgo'
OLD=ROOT/'build-clang-lto-comparison'
PREFIX='C:/Users/bebe/CLionProjects/OpenTTD-patches/cmake-build-release-visual-studio/vcpkg_installed/x64-windows-static'
if not (BASE/'input').exists():shutil.copytree(OLD/'input',BASE/'input')
shutil.copy2(OLD/'host.json',BASE/'host.json')
shared=BASE/'measurement'
for folder in ('save-integrity','real-map-driver','cpu-measurement'):
    (shared/folder).mkdir(parents=True,exist_ok=True)
    for source in (OLD/'thin'/folder).iterdir():
        if source.is_file():
            value=source.read_text(encoding='utf-8').replace('build-clang-lto-comparison/thin','build-clang-thin-pgo/measurement')
            target=shared/folder/source.name
            if not target.exists() or target.read_text(encoding='utf-8')!=value:target.write_text(value,encoding='utf-8')
overlay=(OLD/'thin/measurement.cmake').read_text().replace('build-clang-lto-comparison/thin','build-clang-thin-pgo/measurement')
(BASE/'measurement.cmake').write_text(overlay,encoding='utf-8')
shutil.copy2(OLD/'thin/cpu-sampled.wpaProfile',BASE/'cpu-sampled.wpaProfile')

def configure(mode):
    build=BASE/mode;build.mkdir(exist_ok=True)
    shutil.copy2(BASE/'cpu-sampled.wpaProfile',build/'cpu-sampled.wpaProfile')
    pgo={'baseline':'OFF','generate':'GENERATE','pgo':'USE'}[mode]
    linker='/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEFAULTLIB:"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/lib/clang/19/lib/windows/clang_rt.builtins-x86_64.lib"'
    command=['cmake','-S',str(ROOT),'-B',str(build),'-G','Visual Studio 17 2022','-A','x64','-T','ClangCL,host=x64',
        '-DCMAKE_GENERATOR_INSTANCE=C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools',
        '-DCMAKE_PREFIX_PATH='+PREFIX,'-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
        '-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7','-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7',
        '-DEDITBIN_DIRECTORY=C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64',
        '-DOPTION_USE_ASSERTS=OFF','-DOPTION_PASSENGER_BENCHMARK=ON','-DOPTION_NO_LTO=OFF',
        '-DCMAKE_PROJECT_INCLUDE='+(BASE/'measurement.cmake').as_posix(),'-DCMAKE_EXE_LINKER_FLAGS='+linker,
        '-DOPTION_PGO='+pgo]
    if mode=='pgo':command+=['-DPGO_PROFILE_FILE='+(BASE/'profiles/real-map.profdata').as_posix()]
    (build/'configure-command.json').write_text(json.dumps(command,indent=2))
    with (BASE/f'configure-{mode}.log').open('wb') as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
    print('CONFIGURED',mode,flush=True)
with ThreadPoolExecutor(max_workers=2) as pool:list(pool.map(configure,sys.argv[1:] or ['baseline','generate']))
