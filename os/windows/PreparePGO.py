#!/usr/bin/env python3
"""Prepare/replay a shared Windows profile manually, never from CI.

prepare BUILD writes an opt-in null-driver overlay; train BUILD PACKAGE SAVE
replays 4096 world ticks using that build and merges a distributable profile.
Game simulation translation units are compiled directly from src/ in both modes.
"""
from pathlib import Path
import argparse, hashlib, json, os, re, shutil, subprocess

ROOT = Path(__file__).resolve().parents[2]

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def merge_profile(tool, files, output, work):
    # Also accepts older raw profiles generated before portable names were
    # enabled. Text serialization resolves indirect-call targets to names;
    # re-indexing computes their GUIDs again after normalizing local names.
    text_file=work/'profile.txt'
    subprocess.run([str(tool),'merge','--text',*[str(p) for p in files],'-o',str(text_file)],check=True)
    text=text_file.read_text(encoding='utf-8')
    text=re.sub(r'(?m)^(?:[^;\r\n]*[\\/])([^;\\/\r\n]+);',r'\1;',text)
    lines=text.splitlines()
    first=next(i for i,line in enumerate(lines) if line and not line.startswith(('#',':')))
    header='\n'.join(lines[:first])+'\n'
    records=re.split(r'\n\s*\n','\n'.join(lines[first:]).strip())
    excluded=[]
    kept=[]
    for record in records:
        name=record.splitlines()[0]
        # These two functions differ in the opt-in training harness. Global
        # initializer names also collide for e.g. command.cpp and tests/command.cpp;
        # their startup-only counts are not useful to this simulation profile.
        if any(pattern in name for pattern in ('?MainLoop@VideoDriver_Null@@','?ConUnpauseGame@@','?GetKernel32Function@@',
                   '?OTTDStringCompare@@','?GetFindNLSStringEx@@','?PerThreadSetupInit@@','?SetCurrentThreadName@@','_GLOBAL__sub_I_')):
            excluded.append(name)
        else:
            kept.append(record)
    text=header+'\n\n'.join(kept)+'\n'
    text_file.write_text(text,encoding='utf-8')
    subprocess.run([str(tool),'merge',str(text_file),'-o',str(output)],check=True)
    return dict(excluded_records=len(excluded),excluded_patterns=['VideoDriver_Null::MainLoop','ConUnpauseGame','GetKernel32Function',
                'OTTDStringCompare','GetFindNLSStringEx','PerThreadSetupInit','SetCurrentThreadName','_GLOBAL__sub_I_'])

def prepare(build):
    driver = build / 'pgo-driver'
    driver.mkdir(parents=True, exist_ok=True)
    code = (ROOT / 'src/video/null_v.cpp').read_text(encoding='utf-8')
    def include(match):
        return '#include "' + (ROOT/'src/video'/match[1]).resolve().as_posix() + '"'
    code = re.sub(r'#include "([^"]+)"', include, code)
    headers = '\n'.join('#include "'+(ROOT/'src'/name).as_posix()+'"' for name in
                        ('date_func.h', 'openttd.h', 'progress.h', 'passenger_service_database.h', 'console_func.h'))
    code = code.replace('#include <atomic>', '#include <atomic>\n#include <fstream>\n#include <cstdlib>\n'+headers)
    anchor = '\tSetSelfAsGameThread();'
    assert code.count(anchor) == 1
    code = code.replace(anchor, anchor + '''
    if (const char *result = std::getenv("OPENTTD_PGO_RESULT")) {
        bool started = false;
        int64_t begin = 0;
        while (!_exit_game) {
            if (_game_mode == GameMode::Normal && !HasModalProgress()) {
                if (_pause_mode.Test(PauseMode::Error) || _pause_mode.Test(PauseMode::Normal)) IConsoleCmdExec("unpause");
                if (!started) { begin = _state_ticks.base(); started = true; }
                if (_state_ticks.base() - begin >= 4096) {
                    std::ofstream out(result);
                    out << "{\\"start_tick\\":" << begin << ",\\"end_tick\\":" << _state_ticks.base() << "}";
                    return;
                }
            }
            ::GameLoop();
            ::InputLoop();
            ::UpdateWindows();
        }
        return;
    }
''')
    (driver/'null_v.cpp').write_text(code, encoding='utf-8')
    overlay = '''function(use_training_driver)
  get_target_property(sources ${OPENTTD_LIB} SOURCES)
  list(FILTER sources EXCLUDE REGEX "(^|/)video/null_v\\\\.cpp$")
  set_property(TARGET ${OPENTTD_LIB} PROPERTY SOURCES "${sources}")
  target_sources(${OPENTTD_LIB} PRIVATE "DRIVER/null_v.cpp")
endfunction()
cmake_language(DEFER CALL use_training_driver)
'''.replace('DRIVER',driver.as_posix())
    (build/'pgo-driver.cmake').write_text(overlay, encoding='utf-8')
    print('Configure GENERATE with OPTION_PASSENGER_BENCHMARK=ON and CMAKE_PROJECT_INCLUDE='+str(build/'pgo-driver.cmake'))

def train(build, package, save):
    commands=json.loads((build/'compile_commands.json').read_text(encoding='utf-8'))
    game=next(c['command'] for c in commands if c['file'].replace('\\','/').endswith('/src/openttd.cpp'))
    if not all(flag in game for flag in ('-fprofile-generate','-fprofile-update=atomic','static-func-full-module-prefix=false','-fmacro-prefix-map=','=/openttd')):
        raise ValueError('Use the GENERATE Ninja build with PGO_PORTABLE_NAMES=ON and canonical macro-prefix maps')
    work = build/'pgo-training'
    work.mkdir(exist_ok=False)
    raw = work/'raw'
    raw.mkdir()
    for name in ('baseset','newgrf','content_download','ai','game'):
        if (package/name).exists(): shutil.copytree(package/name,work/name)
    shutil.copytree(build/'lang',work/'lang')
    shutil.copy2(build/'openttd.exe',work/'openttd.exe')
    shutil.copy2(package/'openttd.cfg',work/'openttd.cfg')
    shutil.copy2(save,work/'input.sav')
    settings=(work/'openttd.cfg').read_text(encoding='utf-8')
    assert re.search(r'autosave_on_exit\s*=\s*false',settings), 'Training config must disable exit autosave'
    result = work/'ticks.json'
    env=dict(os.environ, LLVM_PROFILE_FILE=str(raw/'world-%p-%m.profraw'),OPENTTD_PGO_RESULT=str(result))
    command=[str(work/'openttd.exe'),'-c',str(work/'openttd.cfg'),'-x','-v','null:until_exit','-s','null','-m','null','-g',str(work/'input.sav')]
    with (work/'run.log').open('wb') as log:
        subprocess.run(command,cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=2400,check=True)
    ticks=json.loads(result.read_text())
    assert ticks['end_tick']-ticks['start_tick']==4096
    files=sorted(raw.glob('*.profraw'))
    assert files
    output=ROOT/'cmake/pgo'
    output.mkdir(exist_ok=True)
    tool=Path(os.environ['ProgramFiles'])/'LLVM/bin/llvm-profdata.exe'
    filtering=merge_profile(tool,files,output/'windows.profdata',work)
    version=subprocess.check_output([str(tool),'--version'],text=True)
    source_hash=hashlib.sha256()
    for name in subprocess.check_output(['git','ls-files','src'],cwd=ROOT,text=True).splitlines():
        source_hash.update(name.encode()+b'\0')
        source_hash.update((ROOT/name).read_bytes())
    metadata=dict(llvm=version,training_architecture='x86_64-pc-windows-msvc',ticks=ticks,
                  input_sha256=sha(save),profile_sha256=sha(output/'windows.profdata'),
                  source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                  source_files_sha256=source_hash.hexdigest(),
                  arena_allocator_sha256=sha(ROOT/'src/core/arena_alloc.hpp'),
                  driver_sha256=sha(build/'pgo-driver/null_v.cpp'),
                  static_function_prefix='basename (PGO_PORTABLE_NAMES=ON)',
                  source_prefix_map='/openttd (including Microsoft ABI anonymous namespaces)',
                  filtering=filtering,
                  note='Shared IR profile; game simulation sources match the shipping build, including the recorded allocator fix. Training-only driver/unpause functions and global initializer records are excluded. No savegame or assets are distributed.')
    (output/'windows.json').write_text(json.dumps(metadata,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(metadata,indent=2))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode',choices=('prepare','train'))
    parser.add_argument('build',type=Path)
    parser.add_argument('package',type=Path,nargs='?')
    parser.add_argument('save',type=Path,nargs='?')
    args=parser.parse_args()
    if args.mode=='prepare': prepare(args.build.resolve())
    else:
        if not args.package or not args.save: parser.error('train requires PACKAGE and SAVE')
        train(args.build.resolve(),args.package.resolve(),args.save.resolve())
