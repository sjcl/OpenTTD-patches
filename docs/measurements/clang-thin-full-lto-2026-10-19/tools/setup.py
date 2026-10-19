from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import shutil, subprocess, json

ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'build-clang-lto-comparison'
OLD=ROOT/'build-compiler-lto-comparison'
PREFIX='C:/Users/bebe/CLionProjects/OpenTTD-patches/cmake-build-release-visual-studio/vcpkg_installed/x64-windows-static'
for name in ('host.json','cpu-sampled.wprp'):
    shutil.copy2(OLD/name,BASE/name)
if not (BASE/'input').exists(): shutil.copytree(OLD/'input',BASE/'input')

def configure(mode):
    build=BASE/mode;build.mkdir(exist_ok=True)
    for folder in ('save-integrity','real-map-driver','cpu-measurement'):
        (build/folder).mkdir(exist_ok=True)
        for source in (OLD/'clang'/folder).iterdir():
            if source.is_file():
                text=source.read_text(encoding='utf-8').replace('build-compiler-lto-comparison/clang',f'build-clang-lto-comparison/{mode}')
                (build/folder/source.name).write_text(text,encoding='utf-8')
    shutil.copy2(OLD/'clang/cpu-sampled.wpaProfile',build/'cpu-sampled.wpaProfile')
    overlay=(OLD/'clang/measurement.cmake').read_text().replace('build-compiler-lto-comparison/clang',f'build-clang-lto-comparison/{mode}')
    if mode=='full':
        overlay=overlay.replace('-flto=thin','-flto').replace('"$<$<CONFIG:Release>:/lldltocache:${CMAKE_BINARY_DIR}/thinlto-cache>" ','')
    (build/'measurement.cmake').write_text(overlay)
    linker='/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEFAULTLIB:"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/lib/clang/19/lib/windows/clang_rt.builtins-x86_64.lib"'
    command=['cmake','-S',str(ROOT),'-B',str(build),'-G','Visual Studio 17 2022','-A','x64','-T','ClangCL,host=x64',
        '-DCMAKE_GENERATOR_INSTANCE=C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools',
        '-DCMAKE_PREFIX_PATH='+PREFIX,'-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded','-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7','-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7',
        '-DEDITBIN_DIRECTORY=C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64',
        '-DOPTION_USE_ASSERTS=OFF','-DOPTION_PASSENGER_BENCHMARK=ON','-DOPTION_NO_LTO=OFF',
        '-DCMAKE_PROJECT_INCLUDE='+(build/'measurement.cmake').as_posix(),'-DCMAKE_EXE_LINKER_FLAGS='+linker]
    (build/'configure-command.json').write_text(json.dumps(command,indent=2))
    with (BASE/f'configure-{mode}.log').open('wb') as log:
        subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
    print('CONFIGURED',mode,flush=True)
with ThreadPoolExecutor(max_workers=2) as pool: list(pool.map(configure,('thin','full')))
