from pathlib import Path
import subprocess,json,shutil,hashlib,time
BASE=Path(__file__).resolve().parent
ROOT=BASE.parent
fixture=BASE/'invalidation';fixture.mkdir(exist_ok=False)
(fixture/'main.cpp').write_text('int main() { return 0; }\n')
(fixture/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.16)\nproject(PgoInvalidation LANGUAGES CXX)\nset(OPENTTD_LIB openttd)\nadd_executable(openttd main.cpp)\ninclude("'+(ROOT/'cmake/ProfileGuidedOptimization.cmake').as_posix()+'")\nconfigure_pgo()\n')
current=fixture/'current.profdata';source=BASE/'profiles/real-map.profdata'
shutil.copy2(source,current)
build=fixture/'build'
configure=['cmake','-S',str(fixture),'-B',str(build),'-G','Visual Studio 17 2022','-A','x64','-T','ClangCL,host=x64','-DOPTION_PGO=USE','-DPGO_PROFILE_FILE='+current.as_posix()]
command=['cmake','--build',str(build),'--config','Release','--target','openttd','--','/nodeReuse:false']
def run(cmd,name):
    with (fixture/name).open('wb') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
run(configure,'configure.log');run(command,'build-first.log')
obj=build/'openttd.dir/Release/main.obj'
old_time=obj.stat().st_mtime_ns;first=sha(current)
assert (build/'pgo'/(first+'.profdata')).exists()
time.sleep(1.1)
merge=[r'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/llvm-profdata.exe','merge','--weighted-input=2,'+str(source),'-o',str(current)]
subprocess.run(merge,check=True)
second=sha(current);assert first!=second
run(command,'build-updated.log')
assert (build/'pgo'/(second+'.profdata')).exists()
assert obj.stat().st_mtime_ns>old_time,'Object was not rebuilt after updating the profile at the same path'
project=(build/'openttd.vcxproj').read_text(encoding='utf-8-sig')
assert second+'.profdata' in project and first+'.profdata' not in project
subprocess.run([str(build/'Release/openttd.exe')],check=True)
(BASE/'profile-invalidation.json').write_text(json.dumps(dict(configure_command=configure,build_command=command,merge_command=merge,
    initial_profile_sha256=first,updated_profile_sha256=second,initial_object_mtime_ns=old_time,updated_object_mtime_ns=obj.stat().st_mtime_ns,
    auto_reconfigure=True,object_recompiled=True,executable_exit_code=0),indent=2)+'\n')
print('PASS: updating a valid profile at the same path automatically reconfigures CMake, changes the snapshot and recompiles the object; executable passes')
