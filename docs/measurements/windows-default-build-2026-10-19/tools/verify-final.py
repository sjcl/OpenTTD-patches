from pathlib import Path
import json,hashlib,subprocess,ctypes,re,os
ROOT=Path(__file__).resolve().parent.parent
BASE=ROOT/'build-windows-checks'
BUILD=ROOT/'build-windows-default/release'
LLVM=Path(os.environ['ProgramFiles'])/'LLVM/bin'
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
profile=ROOT/'cmake/pgo/windows.profdata'
metadata=json.loads((profile.parent/'windows.json').read_text())
assert sha(profile)==metadata['profile_sha256']
assert metadata['ticks']['end_tick']-metadata['ticks']['start_tick']==4096
assert metadata['input_sha256']==sha(ROOT/'build-clang-thin-pgo/input/input.sav')
commands=json.loads((BUILD/'compile_commands.json').read_text())
for entry in commands:
    if entry['file'].endswith('.cpp') and ('openttd_lib.dir' in entry['output'] or 'openttd_test.dir' in entry['output'] or 'openttd.dir' in entry['output']):
        flags=entry['command']
        assert '-flto=thin' in flags and '-fprofile-use=' in flags and 'static-func-full-module-prefix=false' in flags
        assert '-fprofile-generate' not in flags and 'WITH_PASSENGER_BENCHMARK' not in flags
cache=(BUILD/'CMakeCache.txt').read_text()
assert 'OPTION_PGO:STRING=USE' in cache and 'OPTION_PASSENGER_BENCHMARK:BOOL=OFF' in cache
assert not list(BASE.glob('final-*.profraw'))
portable=json.loads((BASE/'portable-8/result.json').read_text())
assert portable['passed'] and portable['relocated_static_function_profile']
copy=BASE/'relocated-game-source';copy.mkdir(exist_ok=True)
(copy/'src').mkdir(exist_ok=True)
argv_api=ctypes.windll.shell32.CommandLineToArgvW
argv_api.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)]
argv_api.restype=ctypes.POINTER(ctypes.c_wchar_p)
ctypes.windll.kernel32.LocalFree.argtypes=[ctypes.c_void_p]
entries={}
anonymous_entries=[]
for filename in ('openttd.cpp','passenger_precomputed.cpp'):
    command=next(c for c in commands if c['file'].replace('\\','/').endswith('/src/'+filename))
    new_source=copy/'src'/filename;new_source.write_bytes((ROOT/'src'/filename).read_bytes())
    count=ctypes.c_int();ptr=argv_api(command['command'],ctypes.byref(count))
    args=[ptr[i] for i in range(count.value)];ctypes.windll.kernel32.LocalFree(ptr)
    obj=copy/(filename+'.obj')
    for idx,arg in enumerate(args):
        if arg.lower().startswith('/fo'):args[idx]='/Fo'+str(obj)
        elif arg.replace('\\','/').endswith('/src/'+filename):args[idx]=str(new_source)
        elif arg.startswith('-fmacro-prefix-map='):
            args[idx]=arg.replace(str(ROOT),str(copy)).replace(ROOT.as_posix(),copy.as_posix())
    args.insert(args.index('-c'),'/I'+str(ROOT/'src'))
    with (copy/(filename+'.log')).open('wb') as log:subprocess.run(args,cwd=BUILD,stdout=log,stderr=subprocess.STDOUT,check=True)
    ir=copy/(filename+'.ll')
    subprocess.run([str(LLVM/'clang.exe'),'-S','-emit-llvm','-x','ir',str(obj),'-o',str(ir)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
    code=ir.read_text(encoding='utf-8');assert 'ProfileSummary' in code
    for line in code.splitlines():
        if not line.startswith('define '):continue
        ref=re.search(r'!prof !(\d+)',line)
        if not ref:continue
        entry=re.search(r'^!'+ref[1]+r' = !\{!"function_entry_count", i64 (\d+)',code,re.M)
        if not entry:continue
        for name in ('StateGameLoop','GameLoop'):
            if ('?'+name+'@@') in line:
                assert int(entry[1])>0;entries[name]=int(entry[1])
        for name in ('Plan','PrepareGraph'):
            if ('?'+name+'@PassengerPrecomputedTable@@') in line:
                assert int(entry[1])>0;entries['PassengerPrecomputedTable::'+name]=int(entry[1])
        if filename=='passenger_precomputed.cpp' and '?A0x310E4756' in line:
            # The retained SectionGap body has a measured zero count. Hot
            # anonymous helpers were inlined; the separate noinline fixture
            # verifies a positive anonymous-function count and indirect calls.
            anonymous_entries.append(dict(definition=line[:240],count=int(entry[1])))
assert entries['StateGameLoop']==4096 and entries['PassengerPrecomputedTable::Plan']>0 and anonymous_entries
summary=dict(passed=True,profile_sha256=sha(profile),exe_sha256=sha(BUILD/'openttd.exe'),
    pdb_sha256=sha(BUILD/'openttd.pdb'),unit_exe_sha256=sha(BUILD/'openttd_test.exe'),
    relocated_game_profile_entries=entries,relocated_precomputed_anonymous_entries=anonymous_entries,portable_static_function_fixture=portable,
    release_has_thinlto_and_pgo=True,shipping_instrumentation=False,
    source_commit=metadata['source_commit'],compiler=subprocess.check_output([str(LLVM/'clang-cl.exe'),'--version'],text=True))
(BASE/'final-verification.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
