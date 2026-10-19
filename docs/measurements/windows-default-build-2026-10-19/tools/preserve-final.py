from pathlib import Path
import json,hashlib,gzip,shutil,re,subprocess
ROOT=Path(__file__).resolve().parent.parent
CHECK=ROOT/'build-windows-checks';BUILD=ROOT/'build-windows-default/release'
OUT=ROOT/'docs/measurements/windows-default-build-2026-10-19';OUT.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def copy(source,name):
    target=OUT/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
def zipcopy(source,name):
    target=OUT/(name+'.gz');target.parent.mkdir(parents=True,exist_ok=True)
    with source.open('rb') as a,target.open('wb') as b:
        with gzip.GzipFile(filename='',fileobj=b,mode='wb',mtime=0) as z:shutil.copyfileobj(a,z)
log=(ROOT/'build-windows-llvm-download/build-release.log').read_text(encoding='utf-8',errors='replace')
assert '100% tests passed, 0 tests failed out of 784' in log
assert 'function control flow change detected (hash mismatch)' not in log
metadata=json.loads((ROOT/'cmake/pgo/windows.json').read_text())
assert sha(ROOT/'cmake/pgo/windows.profdata')==metadata['profile_sha256']
checks=json.loads((CHECK/'final-verification.json').read_text())
assert checks['passed'] and checks['profile_sha256']==metadata['profile_sha256']
assert checks['exe_sha256']==sha(BUILD/'openttd.exe')
assert not list(CHECK.glob('final-*.profraw'))
work=ROOT/'build-windows-default/generate/pgo-training'
assert not list(work.glob('crash*'))
copy(CHECK/'final-verification.json','verification.json')
copy(ROOT/'cmake/pgo/windows.json','profile-identity.json')
copy(work/'ticks.json','training-ticks.json')
copy(CHECK/'portable-8/result.json','portable/result.json')
for mode in ('original','relocated source'):
    for name in ('CMakeLists.txt','main.cpp'):copy(CHECK/'portable-8'/mode/'source'/name,'portable/'+mode+'/'+name)
    zipcopy(CHECK/'portable-8'/(mode+'.log'),'portable/'+mode+'.log')
for name in ('check-portable.py','verify-final.py','migrate-namespace-hashes.py','preserve-final.py'):
    copy(CHECK/name,'tools/'+name)
zipcopy(ROOT/'build-windows-llvm-download/build-release.log','build-and-ctest.log')
zipcopy(CHECK/'arena-tests.log','arena-exit-regression.log')
zipcopy(CHECK/'test-hang-stacks.log','previous-shutdown-deadlock-stacks.log')
copy(CHECK/'namespace-migration.log','namespace-migration.log')
commands=json.loads((BUILD/'compile_commands.json').read_text())
selected=[x for x in commands if x['file'].replace('\\','/').endswith(('/src/openttd.cpp','/src/passenger_precomputed.cpp','/src/tests/test_main.cpp'))]
(OUT/'compile-commands.json').write_text(json.dumps(selected,indent=2)+'\n')
cache=(BUILD/'CMakeCache.txt').read_text()
settings=[line for line in cache.splitlines() if line.startswith(('CMAKE_CXX_COMPILER:','CMAKE_C_COMPILER:','CMAKE_LINKER:','CMAKE_BUILD_TYPE:','CMAKE_CXX_FLAGS_RELEASE:','CMAKE_MSVC_RUNTIME_LIBRARY:','OPTION_PGO:','PGO_PROFILE_FILE:','PGO_PORTABLE_NAMES:','OPTION_PASSENGER_BENCHMARK:','OPTION_NO_LTO:','OPTION_USE_ASSERTS:','VCPKG_TARGET_TRIPLET:'))]
(OUT/'cache-settings.txt').write_text('\n'.join(settings)+'\n')
files=['CMakeLists.txt','CMakePresets.json','COMPILING.md',*['cmake/'+n for n in ('WindowsDefaults.cmake','WindowsLLVMVersion.cmake','ProfileGuidedOptimization.cmake','ClangCLRuntime.cmake','LinkTimeOptimization.cmake')],*['os/windows/'+n for n in ('Build.ps1','InstallLLVM.ps1','PreparePGO.py')],'src/core/arena_alloc.hpp','src/os/windows/win32.cpp','src/tests/arena_alloc.cpp','.github/workflows/ci-windows.yml','.github/workflows/release-windows.yml']
identity=dict(base_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
              revision=re.search(r'const char _openttd_revision\[\] = "([^"]+)";',(BUILD/'generated/rev.cpp').read_text())[1],
              sources={name:sha(ROOT/name) for name in files},ctest_passed=784,ctest_seconds=float(re.search(r'Total Test time \(real\) = ([0-9.]+)',log)[1]),
              local_runtime_architecture='x64',remote_actions_executed=False,
              machine_update='LLVM.LLVM 18.1.8 -> 23.1.3; version gate restored at 23.1.3',profile_sha256=metadata['profile_sha256'])
(OUT/'build-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
print(json.dumps(dict(ctest_passed=identity['ctest_passed'],ctest_seconds=identity['ctest_seconds'],profile_sha256=metadata['profile_sha256']),indent=2))
