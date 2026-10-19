from pathlib import Path
import subprocess
ROOT = Path(r'C:\Users\bebe\.codex\worktrees\6d2b\OpenTTD-patches')
BASE = ROOT / 'build-compiler-comparison'
for compiler in ('msvc', 'clang'):
    build = BASE / compiler
    replacements = {
        'console_cmds.cpp': 'save-integrity/console_cmds.cpp',
        'cargopacket.cpp': 'save-integrity/cargopacket.cpp',
        'sl/cargopacket_sl.cpp': 'save-integrity/cargopacket_sl.cpp',
        'video/null_v.cpp': 'real-map-driver/null_v.cpp',
        'os/windows/win32.cpp': 'real-map-driver/win32.cpp',
        'openttd.cpp': 'cpu-measurement/openttd.cpp',
        'passenger_journey_active.cpp': 'cpu-measurement/passenger_journey_active.cpp',
        'passenger_service_database.cpp': 'cpu-measurement/passenger_service_database.cpp',
        'passenger_journey_schedule.cpp': 'cpu-measurement/passenger_journey_schedule.cpp',
    }
    lines = ['function(use_compiler_measurement)', 'get_target_property(sources openttd_lib SOURCES)']
    for source, target in replacements.items():
        lines += [f'list(REMOVE_ITEM sources "{(ROOT / "src" / source).as_posix()}")', f'list(APPEND sources "{(build / target).as_posix()}")']
    lines += ['set_property(TARGET openttd_lib PROPERTY SOURCES "${sources}")', 'endfunction()', 'cmake_language(DEFER CALL use_compiler_measurement)']
    overlay = build / 'measurement.cmake'
    overlay.write_text('\n'.join(lines) + '\n')
    command = ['cmake', '-S', str(ROOT), '-B', str(build), '-DCMAKE_PROJECT_INCLUDE=' + overlay.as_posix(), '-DOPTION_NO_LTO=ON']
    linker = '/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO'
    if compiler == 'clang':
        linker += ' /DEFAULTLIB:"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/lib/clang/19/lib/windows/clang_rt.builtins-x86_64.lib"'
    command += ['-DCMAKE_EXE_LINKER_FLAGS=' + linker]
    with (BASE / f'configure-overlay-{compiler}.log').open('w', encoding='utf-8') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    print('Configured', compiler, flush=True)
