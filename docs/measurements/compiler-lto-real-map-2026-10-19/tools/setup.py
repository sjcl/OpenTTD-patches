from pathlib import Path
import shutil, subprocess, json

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT / 'build-compiler-lto-comparison'
OLD = ROOT / 'build-compiler-comparison'
PREFIX = 'C:/Users/bebe/CLionProjects/OpenTTD-patches/cmake-build-release-visual-studio/vcpkg_installed/x64-windows-static'
for name in ('host.json', 'cpu-sampled.wprp'):
    shutil.copy2(OLD/name, BASE/name)
if not (BASE/'input').exists():
    shutil.copytree(OLD/'input',BASE/'input')
for compiler in ('msvc', 'clang'):
    build = BASE/compiler
    build.mkdir(exist_ok=True)
    for folder in ('save-integrity', 'real-map-driver', 'cpu-measurement'):
        (build/folder).mkdir(exist_ok=True)
        for source in (OLD/compiler/folder).iterdir():
            if source.is_file():
                text = source.read_text(encoding='utf-8').replace('build-compiler-comparison', 'build-compiler-lto-comparison')
                (build/folder/source.name).write_text(text, encoding='utf-8')
    shutil.copy2(OLD/compiler/'cpu-sampled.wpaProfile', build/'cpu-sampled.wpaProfile')
    overlay = (OLD/compiler/'measurement.cmake').read_text().replace('build-compiler-comparison', 'build-compiler-lto-comparison')
    if compiler == 'msvc':
        extra = '''
foreach(target openttd openttd_lib)
set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

endforeach()
target_link_options(openttd PRIVATE "$<$<CONFIG:Release>:/LTCG>" "$<$<CONFIG:Release>:/TIME>")
'''
    else:
        extra = '''
foreach(target openttd openttd_lib)
set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
target_compile_options(${target} PRIVATE "$<$<CONFIG:Release>:-flto=thin>")
endforeach()
target_link_options(openttd PRIVATE "$<$<CONFIG:Release>:/lldltocache:${CMAKE_BINARY_DIR}/thinlto-cache>" "$<$<CONFIG:Release>:/time>")
'''
    overlay = overlay.replace('endfunction()', extra+'\nendfunction()')
    (build/'measurement.cmake').write_text(overlay)
    linker = '/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO'
    if compiler == 'clang':
        linker += ' /DEFAULTLIB:"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/lib/clang/19/lib/windows/clang_rt.builtins-x86_64.lib"'
    cmd = ['cmake', '-S', str(ROOT), '-B', str(build), '-G', 'Visual Studio 17 2022', '-A', 'x64',
        '-DCMAKE_GENERATOR_INSTANCE=C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools',
        '-DCMAKE_PREFIX_PATH='+PREFIX, '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
        '-DEDITBIN_DIRECTORY=C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64',
        '-DOPTION_USE_ASSERTS=OFF', '-DOPTION_PASSENGER_BENCHMARK=ON', '-DOPTION_NO_LTO=OFF',
        '-DCMAKE_PROJECT_INCLUDE='+(build/'measurement.cmake').as_posix(), '-DCMAKE_EXE_LINKER_FLAGS='+linker]
    if compiler == 'clang': cmd += ['-T', 'ClangCL,host=x64']
    (build/'configure-command.json').write_text(json.dumps(cmd, indent=2))
    with (BASE/f'configure-{compiler}.log').open('wb') as log:
        subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True)
    print('Configured', compiler, flush=True)
