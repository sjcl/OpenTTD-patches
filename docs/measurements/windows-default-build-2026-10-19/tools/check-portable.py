from pathlib import Path
import subprocess,os,json,re,importlib.util
ROOT=Path(__file__).resolve().parent.parent
BASE=Path(__file__).resolve().parent/'portable-8'
BASE.mkdir(exist_ok=False)
LLVM=Path(os.environ['ProgramFiles'])/'LLVM/bin'
template='''cmake_minimum_required(VERSION 3.20)
include("ROOT/cmake/WindowsDefaults.cmake")
project(profile_portability LANGUAGES CXX)
set(CMAKE_MODULE_PATH "ROOT/cmake")
add_executable(openttd main.cpp)
set(OPENTTD_LIB openttd)
include(LinkTimeOptimization)
configure_lto()
include(ProfileGuidedOptimization)
configure_pgo()
'''.replace('ROOT',ROOT.as_posix())
source='''namespace { __declspec(noinline) int branch(int x) { if(x>4)return x-1; return x+3; } }
int main(){ volatile int x=7; int(*volatile fn)(int)=branch; for(int i=0;i<30;i++)x=fn(x); return 0; }
'''
def configure(name,mode,extra=[]):
    src=BASE/name/'source';build=BASE/name/'build';src.mkdir(parents=True)
    (src/'CMakeLists.txt').write_text(template)
    (src/'main.cpp').write_text(source)
    with (BASE/(name+'.log')).open('wb') as log:
        subprocess.run(['cmake','-S',str(src),'-B',str(build),'-G','Visual Studio 17 2022','-A','x64','-DOPTION_PGO='+mode,*extra],check=True,stdout=log,stderr=subprocess.STDOUT)
        subprocess.run(['cmake','--build',str(build),'--config','Release'],check=True,stdout=log,stderr=subprocess.STDOUT)
    assert not re.search('out of date|hash mismatch', (BASE/(name+'.log')).read_text(encoding='utf-8',errors='replace'),re.I)
    return build
a=configure('original','GENERATE')
raw=BASE/'raw';raw.mkdir()
subprocess.run([str(a/'Release/openttd.exe')],env=dict(os.environ,LLVM_PROFILE_FILE=str(raw/'test-%p.profraw')),check=True)
profile=BASE/'portable.profdata'
spec=importlib.util.spec_from_file_location('prepare_pgo',ROOT/'os/windows/PreparePGO.py')
prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
prepare.merge_profile(LLVM/'llvm-profdata.exe',list(raw.glob('*.profraw')),profile,BASE)
b=configure('relocated source','USE',['-DPGO_PROFILE_FILE='+str(profile)])
subprocess.run([str(b/'Release/openttd.exe')],check=True)
obj=b/'openttd.dir/Release/main.obj';ir=BASE/'relocated.ll'
subprocess.run([str(LLVM/'clang.exe'),'-S','-emit-llvm','-x','ir',str(obj),'-o',str(ir)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
code=ir.read_text();assert 'ProfileSummary' in code and re.search(r'function_entry_count".*, i64 30',code)
summary=subprocess.check_output([str(LLVM/'llvm-profdata.exe'),'show','--all-functions','--counts',str(profile)],text=True)
assert 'main.cpp;' in summary and 'branch' in summary and str(BASE) not in summary
info=dict(passed=True,source_paths=[str(BASE/n/'source') for n in ('original','relocated source')],
          compiler_defaults='Visual Studio generator with no -T or compiler override selected LLVM 23.1.3',
          relocated_static_function_profile=True,profile_summary=True,function_entry_count=True)
(BASE/'result.json').write_text(json.dumps(info,indent=2)+'\n')
print(json.dumps(info,indent=2))
